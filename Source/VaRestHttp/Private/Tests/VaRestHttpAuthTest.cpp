// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "VaRestAuth.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTesting.h"
#include "VaRestRetry.h"

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
		Rig.Drain();
		VaRest::Http::Testing::FlushRedactionPipe();
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
			LastRefreshToken = Context.GetSession().RefreshToken;
			Pending = MoveTemp(Done);
		}

		bool IsPending() const { return static_cast<bool>(Pending); }
		TUniqueFunction<void(FVaRestAuthRefreshResult&&)> TakePending() { return MoveTemp(Pending); }

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
		/** The refresh token the last refresh started from (its context's session) */
		FString LastRefreshToken;
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

	/** The brief's Text(): renamed, as lambdas below name a parameter Text (MSVC C4459) */
	FString BodyText(TConstArrayView<uint8> Bytes)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	/** A refresh endpoint and a protected resource on the fake: the refresh trades OldRefresh for new tokens; the resource wants the new access token */
	void RefreshServer(VaRest::Http::Testing::FTestRig& Rig, FVaRestFakeResponse RefreshAnswer)
	{
		Rig.Fake->SetResponder([RefreshAnswer](const FVaRestTransportRequest& Request)
		{
			if (Request.Url.Contains(TEXT("/auth/")))
			{
				return RefreshAnswer;
			}
			return Request.Headers.Get(TEXT("Authorization")) == TEXT("Bearer new-access-123456") ? Json(200, R"({"ok":true})") : Json(401);
		});
	}

	FVaRestFakeResponse NewTokens()
	{
		return Json(200, R"({"accessToken":"new-access-123456","refreshToken":"new-refresh-123456","expiresIn":60})");
	}

	/** Every token a refresh test uses: none may reach a log line or a trace field */
	const TCHAR* const RefreshTokens[] = { TEXT("new-access-123456"), TEXT("old-access-123456"), TEXT("new-refresh-123456"), TEXT("old-refresh-123456") };

	/**
	 * A PerAttempt stage that holds the refresh request's attempt (a URL with "/auth/") until the test lets it go, and passes
	 * every other. A held attempt holds its exchange, and so the client: release it before the test ends.
	 */
	class FRefreshHold final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("RefreshHold"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			if (!Exchange->GetUrl().Contains(TEXT("/auth/")))
			{
				Next.Proceed(Exchange, MoveTemp(OnOutcome));
				return;
			}
			Held = Exchange;
			HeldNext = Next;
			HeldDone = MoveTemp(OnOutcome);
		}

		bool IsHolding() const { return Held.IsValid(); }

		/** Sends the held attempt on, synchronously; nothing when none is held */
		void Release()
		{
			const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Going = MoveTemp(Held);
			Held.Reset();
			if (Going.IsValid() && HeldNext.IsSet())
			{
				const FVaRestStageNext Next = HeldNext.GetValue();
				HeldNext.Reset();
				FVaRestOutcomeCallback Done = MoveTemp(HeldDone);
				HeldDone.Reset();
				Next.Proceed(Going.ToSharedRef(), MoveTemp(Done));
			}
		}

	private:
		TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Held;
		TOptional<FVaRestStageNext> HeldNext;
		FVaRestOutcomeCallback HeldDone;
	};

	/**
	 * M4a final review B5 (review B m-3): a custom auth with a configuration error whose CanRefresh and NeedsRefresh still
	 * say yes. Its Refresh, should anything call it, answers Failed(Connection) at once, so nothing is left waiting.
	 */
	class FMisconfiguredRefresher final : public IVaRestAuth
	{
	public:
		virtual FName GetAuthName() const override { return TEXT("MisconfiguredRefresher"); }
		virtual FString GetConfigError() const override { return TEXT("the signing key is missing"); }
		virtual bool Apply(FVaRestAuthTarget& Target) const override { return Target.Session.HasAccessToken(); }
		virtual bool CanRefresh(const FVaRestSessionData&) const override { return true; }
		virtual bool NeedsRefresh(const FVaRestSessionData&, double) const override { return true; }
		virtual void Refresh(const FVaRestAuthRefreshContext&, TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done) override
		{
			++Refreshes;
			Done(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Connection, TEXT("refreshed despite the config error")));
		}

		int32 Refreshes = 0;
	};
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthGetRedactorTest, "VaRest.Http.Auth.GetRedactorCoversApplyAuth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthGetRedactorTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// M4a final review B8 (review B m-8): a socket (W5, VaRest-81s) must keep what ApplyAuth wrote redacted, and the keys
	// for it live only in the client's redactor. GetRedactor() is the one the client publishes now: it masks every value
	// ApplyAuth wrote and every name it wrote one under -- names chosen outside the built-in key list (layer 1), so only the
	// client's own keys can mask them -- and the game's own secret. One taken before a logout still knows the old token;
	// the client's current one forgets it at once (Inspector 12). An empty client gives one that knows no client's values.
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Query, TEXT("studio_key"), TEXT("studio-key-value-123456")));
	Rig.Client.SetSession(TEXT("session-access-123456"), TEXT("session-refresh-123456"));
	Rig.Client.SetSessionHeader(TEXT("X-Player-Ticket"), TEXT("player-ticket-123456"));
	Rig.Client.SetSecretValue(TEXT("Game"), TEXT("game-secret-123456"));
	FVaRestHeaders Headers;
	TArray<TPair<FString, FString>> Query;
	if (!TestTrue(TEXT("ApplyAuth writes the key into the query and the session header"), Rig.Client.ApplyAuth(Headers, Query)
		&& Query.Num() == 1 && Headers.Contains(TEXT("X-Player-Ticket"))))
	{
		return false;
	}
	const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Taken = Rig.Client.GetRedactor();
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
	TestTrue(TEXT("the client's published redactor itself"), State.IsValid() && &Taken.Get() == &State->GetRedactor().Get());
	FString Written = FString::Printf(TEXT("%s %s game-secret-123456 session-refresh-123456"), *Query[0].Value, *Headers.Get(TEXT("X-Player-Ticket")));
	const FString Scrubbed = Taken->ScrubText(Written);
	for (const TCHAR* Value : { TEXT("studio-key-value-123456"), TEXT("player-ticket-123456"), TEXT("game-secret-123456"), TEXT("session-refresh-123456") })
	{
		TestFalse(FString::Printf(TEXT("value %s scrubbed"), Value), Scrubbed.Contains(Value));
	}
	TestEqualSensitive(TEXT("the auth's query name is a key (a short value too)"), Taken->RedactUrl(TEXT("wss://api.test/ws?studio_key=short&page=2")),
		FString(TEXT("wss://api.test/ws?studio_key=***&page=2")));
	FVaRestHeaders Short;
	Short.Set(TEXT("X-Player-Ticket"), TEXT("short"));
	TestEqualSensitive(TEXT("the session header's name is a key"), Taken->RedactHeaders(Short).Get(TEXT("X-Player-Ticket")), FString(TEXT("***")));
	Rig.Client.ClearSession();
	TestFalse(TEXT("taken before a logout, it still scrubs the old token"), Taken->ScrubText(TEXT("session-access-123456")).Contains(TEXT("session-access-123456")));
	TestTrue(TEXT("... the client's current one forgot it"), Rig.Client.GetRedactor()->ScrubText(TEXT("session-access-123456")).Contains(TEXT("session-access-123456")));
	const FVaRestClient Empty;
	TestEqualSensitive(TEXT("an empty client: a redactor that knows no client's values"), Empty.GetRedactor()->ScrubText(TEXT("game-secret-123456")),
		FString(TEXT("game-secret-123456")));
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

	// M4a final review B-I-1: Unavailable is RefreshAuth's answer, not an auth's. One that reports it keeps the session, as
	// a Failed does (Task 15 review M-5's rule): never a logout
	const FRefreshAnswers Misused = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Misused));
	FVaRestAuthRefreshResult Unavailable;
	Unavailable.Outcome = EVaRestAuthRefreshOutcome::Unavailable;
	Auth->Answer(MoveTemp(Unavailable));
	TestTrue(TEXT("an auth answering Unavailable: (Failed, Connection)"), PumpUntil([Misused] { return Misused->Num() == 1; })
		&& (*Misused)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Failed, EVaRestErrorKind::Connection));
	TestTrue(TEXT("... the session kept, nothing expired"), Rig.Client.GetSession().HasRefreshToken() && *Expired == 0);

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

	// M4a final review B-I-1: no refresh in flight and none can start. No auth, a Bearer, and a BearerRefresh session with
	// no refresh token are Unavailable -- the session stays, nothing is sent, and the caller's own 401 is the answer -- while
	// a misconfigured auth stays the configuration's (Failed, Config) (ruling Q7). Still never inside the call (decision 3).
	FTestRig Plain;
	const FRefreshAnswers None = MakeAnswers();
	Plain.Client.RefreshAuth(Record(None));
	TestEqual(TEXT("decision 3: nothing inside the call"), None->Num(), 0);
	TestTrue(TEXT("no auth: (Unavailable, None)"), PumpUntil([None] { return None->Num() == 1; })
		&& (*None)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Unavailable, EVaRestErrorKind::None));

	FTestRig Bearer;
	Bearer.Client.WithBearer(TEXT("bearer-token-123456"));
	const FRefreshAnswers Plainly = MakeAnswers();
	Bearer.Client.RefreshAuth(Record(Plainly));
	TestTrue(TEXT("Bearer: (Unavailable, None)"), PumpUntil([Plainly] { return Plainly->Num() == 1; })
		&& (*Plainly)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Unavailable, EVaRestErrorKind::None));
	TestTrue(TEXT("... the token kept"), Bearer.Client.GetSession().AccessToken.Equals(TEXT("bearer-token-123456"), ESearchCase::CaseSensitive));

	FTestRig NoRefreshToken;
	NoRefreshToken.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	NoRefreshToken.Client.SetBearerToken(TEXT("access-only-123456"));
	const FRefreshAnswers AccessOnly = MakeAnswers();
	NoRefreshToken.Client.RefreshAuth(Record(AccessOnly));
	TestTrue(TEXT("BearerRefresh with no refresh token: (Unavailable, None)"), PumpUntil([AccessOnly] { return AccessOnly->Num() == 1; })
		&& (*AccessOnly)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Unavailable, EVaRestErrorKind::None));
	TestTrue(TEXT("... nothing sent, the token kept"), NoRefreshToken.Fake->NumCalls() == 0
		&& NoRefreshToken.Client.GetSession().AccessToken.Equals(TEXT("access-only-123456"), ESearchCase::CaseSensitive));

	AddExpectedErrorPlain(TEXT("FVaRestAuth::BearerRefresh: AccessTokenPath is empty"), EAutomationExpectedErrorFlags::Contains, 1);
	FVaRestBearerRefreshConfig Broken;
	Broken.AccessTokenPath.Reset();
	FTestRig Misconfigured;
	Misconfigured.Client.WithAuth(FVaRestAuth::BearerRefresh(Broken));
	Misconfigured.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	const FRefreshAnswers Typo = MakeAnswers();
	Misconfigured.Client.RefreshAuth(Record(Typo));
	TestTrue(TEXT("a misconfigured BearerRefresh: (Failed, Config)"), PumpUntil([Typo] { return Typo->Num() == 1; })
		&& (*Typo)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Failed, EVaRestErrorKind::Config));
	TestEqual(TEXT("... nothing sent"), Misconfigured.Fake->NumCalls(), 0);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthParkOwnerPollTest, "VaRest.Http.Auth.ParkedOwnerRetirement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthParkOwnerPollTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	class FTrackingClock final : public IVaRestClock
	{
	public:
		explicit FTrackingClock(TSharedRef<IVaRestClock, ESPMode::ThreadSafe> InClock) : Clock(MoveTemp(InClock)) {}
		virtual double Now() const override { return Clock->Now(); }
		virtual FVaRestTimerHandle SetTimer(double Delay, TUniqueFunction<void()>&& Callback) override
		{
			const auto Id = MakeShared<uint64, ESPMode::ThreadSafe>(0);
			const auto Handle = Clock->SetTimer(Delay, [State = Pending, Id, Original = MoveTemp(Callback)]() mutable {
				State->Remove(*Id);
				Original();
			});
			if (Handle.IsValid())
			{
				*Id = Handle.Id;
				Pending->Add(Handle.Id, Delay);
			}
			return Handle;
		}
		virtual void ClearTimer(FVaRestTimerHandle& Handle) override
		{
			Pending->Remove(Handle.Id);
			Clock->ClearTimer(Handle);
		}
		int32 NumPendingAtMost(double Delay) const
		{
			int32 Count = 0;
			for (const auto& Entry : *Pending)
				Count += Entry.Value <= Delay;
			return Count;
		}
	private:
		TSharedRef<IVaRestClock, ESPMode::ThreadSafe> Clock;
		TSharedRef<TMap<uint64, double>, ESPMode::ThreadSafe> Pending = MakeShared<TMap<uint64, double>, ESPMode::ThreadSafe>();
	};
	for (bool bReplay : { false, true })
	{
		for (bool bOtherWaiter : { false, true })
		{
			FTestRig Rig;
			const auto Auth = HandRig(Rig);
			if (!bReplay)
				Rig.Client.SetSession(TEXT(""), TEXT("old-refresh-123456"));
			const auto Clock = MakeShared<FTrackingClock, ESPMode::ThreadSafe>(Rig.Clock);
			Rig.Client.WithClock(Clock);
			ON_SCOPE_EXIT { Abandon(Rig); Auth->Drop(); };
			const auto Probe = MakeShared<FOutcomeProbe, ESPMode::ThreadSafe>();
			Rig.Client.Use(Probe, EVaRestStageSlot::Outer);
			const auto State = VaRest::Http::FClientAccess::State(Rig.Client);
			UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
			TStrongObjectPtr<UObject> KeepOwner(Owner);
			auto Owned = MakeOutcome<FVaRestJson>();
			auto Other = MakeOutcome<FVaRestJson>();
			TSharedPtr<int32, ESPMode::ThreadSafe> Capture = MakeShared<int32, ESPMode::ThreadSafe>(7);
			const TWeakPtr<int32, ESPMode::ThreadSafe> WeakCapture = Capture;
			const auto OwnedHandle = Observe(Rig.Client.Get(TEXT("/owned")).Owner(Owner), Owned)
				.OnError([Owned, Keep = Capture](const FVaRestError&) { (void)Keep; Owned->ErrorCalls.fetch_add(1); }).Send();
			Capture.Reset();
			if (bOtherWaiter)
				Observe(Rig.Client.Get(TEXT("/other")), Other).Send();
			const int32 Waiters = bOtherWaiter ? 2 : 1;
			if (!TestTrue(TEXT("requests park behind a provider still holding Done"),
				PumpUntil([&] { return Auth->IsPending() && State->NumRefreshWaitersInternal() == Waiters; })))
				return false;
			TestTrue(TEXT("the owned callback payload is retained while its owner lives"), WeakCapture.IsValid());
			TestEqual(TEXT("the held owned park has one short owner timer"), Clock->NumPendingAtMost(0.1), 1);
			const int32 InitialCalls = bReplay ? Waiters : 0;
			TestEqual(TEXT("proactive or reactive physical attempts"), Rig.Fake->NumCalls(), InitialCalls);
			KeepOwner.Reset();
			Owner->MarkAsGarbage();
			Rig.Clock->Advance(0.1);
			const int32 Remaining = bOtherWaiter ? 1 : 0;
			// Do not globally Drain while the other logical request is deliberately still parked.
			if (!TestTrue(TEXT("owner retirement completes before the refresh provider answers"), PumpUntil([&] {
				return State->NumRefreshWaitersInternal() == Remaining && Rig.Client.GetInFlightCount() == Remaining && !WeakCapture.IsValid();
			})))
				return false;
			TestTrue(TEXT("the provider is still holding its Done"), Auth->IsPending());
			TestEqual(TEXT("owner retirement clears the short timer before Done"), Clock->NumPendingAtMost(0.1), 0);
			TestEqual(TEXT("the outer stage observes one cancellation"), Probe->Count(OwnedHandle.GetId(), EVaRestTransportFailure::Canceled), 1);
			TestEqual(TEXT("the owned request has one stage outcome"), Probe->Total(OwnedHandle.GetId()), 1);
			TestEqual(TEXT("owner retirement is silent"), Owned->SuccessCalls.load() + Owned->ErrorCalls.load() + Owned->CompleteCalls.load(), 0);
			TestEqual(TEXT("no further dispatch during the held refresh"), Rig.Fake->NumCalls(), InitialCalls);
			Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
			if (bOtherWaiter && !TestTrue(TEXT("the unowned waiter survives and succeeds"), Rig.Wait(Other) && Other->SuccessCalls.load() == 1))
				return false;
			if (!TestTrue(TEXT("all requests have ended"), PumpUntil([&] { return Rig.Client.GetInFlightCount() == 0; })))
				return false;
			TestTrue(TEXT("terminal requests drain"), Rig.Drain());
			TestEqual(TEXT("only the surviving waiter dispatches"), Rig.Fake->NumCalls(), InitialCalls + Remaining);
			// Rotated token redaction legitimately keeps a ten-minute secret-expiry timer.
			TestEqual(TEXT("the owner poll leaves no short timer behind"), Clock->NumPendingAtMost(0.1), 0);
			TestEqual(TEXT("a late refresh does not answer the owned stage again"), Probe->Total(OwnedHandle.GetId()), 1);
		}
	}
	for (bool bReplay : { false, true })
	{
		FTestRig Rig;
		const auto Auth = HandRig(Rig);
		if (!bReplay)
			Rig.Client.SetSession(TEXT(""), TEXT("old-refresh-123456"));
		const auto Clock = MakeShared<FTrackingClock, ESPMode::ThreadSafe>(Rig.Clock);
		Rig.Client.WithClock(Clock);
		ON_SCOPE_EXIT { Abandon(Rig); Auth->Drop(); };
		TStrongObjectPtr<UObject> Owner(NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient));
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/alive")).Owner(Owner.Get()), Outcome).Send();
		if (!TestTrue(TEXT("the living owner also parks"), PumpUntil([&] { return Auth->IsPending(); })))
			return false;
		Rig.Clock->Advance(0.1); // exercises rearming while the owner remains valid
		TestEqual(TEXT("the living request remains in flight"), Rig.Client.GetInFlightCount(), 1);
		TestEqual(TEXT("the living park rearms exactly one short timer"), Clock->NumPendingAtMost(0.1), 1);
		Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
		if (!TestTrue(TEXT("the living owner receives its result"), Rig.Wait(Outcome) && Outcome->SuccessCalls.load() == 1))
			return false;
		TestTrue(TEXT("the terminal living request drains"), Rig.Drain());
		TestEqual(TEXT("normal refresh completion clears the short owner timer"), Clock->NumPendingAtMost(0.1), 0);
	}
	{
		class FRefuseTimers final : public IVaRestClock
		{
		public:
			explicit FRefuseTimers(TSharedRef<IVaRestClock, ESPMode::ThreadSafe> InClock) : Clock(MoveTemp(InClock)) {}
			virtual double Now() const override { return Clock->Now(); }
			virtual FVaRestTimerHandle SetTimer(double, TUniqueFunction<void()>&&) override { return {}; }
			virtual void ClearTimer(FVaRestTimerHandle& Handle) override { Clock->ClearTimer(Handle); }
		private:
			TSharedRef<IVaRestClock, ESPMode::ThreadSafe> Clock;
		};
		FTestRig Rig;
		const auto Auth = HandRig(Rig);
		Rig.Client.SetSession(TEXT(""), TEXT("old-refresh-123456"));
		Rig.Client.WithClock(MakeShared<FRefuseTimers, ESPMode::ThreadSafe>(Rig.Clock));
		ON_SCOPE_EXIT { Abandon(Rig); Auth->Drop(); };
		TStrongObjectPtr<UObject> Owner(NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient));
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/refused")).Owner(Owner.Get()), Outcome).Send();
		if (!TestTrue(TEXT("a refused owner timer ends the request"), Rig.Wait(Outcome)))
			return false;
		TestTrue(TEXT("lifecycle admission refusal is Config and not retryable"),
			Outcome->Error.Kind == EVaRestErrorKind::Config && !Outcome->Error.bRetryable && Outcome->Error.Message.Contains(TEXT("LifecycleCapacity")));
		TestEqual(TEXT("no refresh starts after timer admission is refused"), Auth->Refreshes, 0);
		TestEqual(TEXT("no transport starts after timer admission is refused"), Rig.Fake->NumCalls(), 0);
		TestEqual(TEXT("no waiter is retained after refusal"), VaRest::Http::FClientAccess::State(Rig.Client)->NumRefreshWaitersInternal(), 0);
		TestTrue(TEXT("the refusal preserves the refresh session"), Rig.Client.GetSession().HasRefreshToken());
		TestTrue(TEXT("the terminal refusal drains"), Rig.Drain());
	}
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthShutdownRefusalTest, "VaRest.Http.Auth.RefreshAuthRefusalAtShutdown",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthShutdownRefusalTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// M4a final review B7 (review B m-7): RefreshAuth's own answer, when nothing can refresh the session, follows decision 2
	// as the refresh's waiters do (ShutdownDropsRefreshWaiters): at engine exit OnDone is destroyed inside the call,
	// uncalled, and no task is launched for it. CancelForShutdown() is process-wide: nothing else of VaRest runs here.
	FTestRig Plain;
	VaRest::Http::FClientState::CancelForShutdown();
	ON_SCOPE_EXIT { VaRest::Http::FClientState::ResetShutdownInternal(); };
	const FRefreshAnswers Answers = MakeAnswers();
	const TSharedRef<int32, ESPMode::ThreadSafe> Held = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Plain.Client.RefreshAuth([Answers, Kept = Held](EVaRestAuthRefreshOutcome Outcome, EVaRestErrorKind FailedKind) { Answers->Emplace(Outcome, FailedKind); });
	TestEqual(TEXT("decision 2: OnDone is gone when the call returns (only this test holds the probe)"), Held.GetSharedReferenceCount(), 1);
	Plain.Drain();
	TestEqual(TEXT("... and is never called"), Answers->Num(), 0);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthParkEndLineTest, "VaRest.Http.Auth.ParkEndWritesOneBasicLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthParkEndLineTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 17 review I-1 (ruling I-3 as spec review I-1 extends it): a park before an attempt is that attempt's Auth phase,
	// so each end that answers it without a dispatch -- a rejected refresh, a failed one, a Cancel() -- writes the
	// attempt's one Basic line: the request's URL, its own 0 ms and the wait behind the refresh. A replay's park is a Gap,
	// as a backoff is: its end adds nothing to the 401 line before it. Nor does a park the request's accepted answer ends.
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	const TSharedRef<FEarlyAnswer, ESPMode::ThreadSafe> Early = MakeShared<FEarlyAnswer, ESPMode::ThreadSafe>();
	Rig.Client.WithLogLevel(EVaRestLogLevel::Basic).Use(Early, EVaRestStageSlot::Outer);
	FLogCapture Capture;
	const auto LinesOf = [&Capture](uint64 Id, const TCHAR* Text)
	{
		const FString Prefix = FString::Printf(TEXT("#%llu "), Id);
		int32 Count = 0;
		for (const FString& Line : Capture.GetLines())
		{
			Count += Line.Contains(Prefix) && Line.Contains(Text) ? 1 : 0;
		}
		return Count;
	};
	int32 Parks = 0;
	const auto ParkDue = [&Rig, &Auth, &Parks](const TCHAR* Path, const TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>& Outcome, FVaRestRequestHandle& OutHandle,
		double ExpiresIn = 20.0)
	{
		// A known expiry within the lead: the request parks before its first attempt (spec 4.2, "proactively"). The game's
		// new access token ends a refresh still in flight (Ready; only a new one does, M4a final review B3), so each park
		// starts its own.
		Rig.Client.SetSession(FString::Printf(TEXT("park-access-%d-123456"), ++Parks), TEXT("old-refresh-123456"), ExpiresIn);
		const int32 Before = Auth->Refreshes;
		OutHandle = Observe(Rig.Client.Get(Path), Outcome).Send();
		return Rig.Drain() && Auth->IsPending() && Auth->Refreshes == Before + 1 && Rig.Fake->NumCalls() == 0;
	};

	FVaRestRequestHandle RejectedHandle;
	auto Rejected = MakeOutcome<FVaRestJson>();
	if (!TestTrue(TEXT("parked before its attempt"), ParkDue(TEXT("/rejected"), Rejected, RejectedHandle)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.25);
	Auth->Answer(FVaRestAuthRefreshResult::Rejected(TEXT("the refresh endpoint answered 400")));
	TestTrue(TEXT("Rejected: Kind Auth"), Rig.Wait(Rejected) && Rejected->Error.Kind == EVaRestErrorKind::Auth);
	TestEqual(TEXT("... one Basic line"), LinesOf(RejectedHandle.GetId(), TEXT("auth wait")), 1);
	TestEqual(TEXT("... the request's URL, the attempt's 0 ms, the 250 ms behind the refresh"),
		LinesOf(RejectedHandle.GetId(), TEXT("GET https://api.test/rejected -> Other AuthRejected (0 ms, 250 ms auth wait)")), 1);

	FVaRestRequestHandle FailedHandle;
	auto Failed = MakeOutcome<FVaRestJson>();
	// A token that expires while it waits: a failed proactive refresh sends one still valid (M4a final review B6)
	if (!TestTrue(TEXT("parked again"), ParkDue(TEXT("/failed"), Failed, FailedHandle, 0.05)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.1);
	Auth->Answer(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Connection, TEXT("the refresh endpoint did not answer")));
	TestTrue(TEXT("Failed: Kind Connection"), Rig.Wait(Failed) && Failed->Error.Kind == EVaRestErrorKind::Connection);
	TestEqual(TEXT("... one Basic line"), LinesOf(FailedHandle.GetId(), TEXT("GET https://api.test/failed -> Connection AuthRefreshFailed (0 ms, 100 ms auth wait)")), 1);

	FVaRestRequestHandle CanceledHandle;
	auto Canceled = MakeOutcome<FVaRestJson>();
	if (!TestTrue(TEXT("parked a third time"), ParkDue(TEXT("/canceled"), Canceled, CanceledHandle)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.05);
	CanceledHandle.Cancel();
	TestTrue(TEXT("Cancel(): Kind Canceled"), Rig.Wait(Canceled) && Canceled->Error.Kind == EVaRestErrorKind::Canceled);
	TestEqual(TEXT("... one Basic line"), LinesOf(CanceledHandle.GetId(), TEXT("GET https://api.test/canceled -> canceled (0 ms, 50 ms auth wait)")), 1);

	// The answer accepted while it is parked (EndWaits ends the park): the decoded answer is delivered, no park line
	FVaRestRequestHandle AcceptedHandle;
	auto Accepted = MakeOutcome<FVaRestJson>();
	if (!TestTrue(TEXT("parked a fourth time"), ParkDue(TEXT("/accepted"), Accepted, AcceptedHandle)))
	{
		Abandon(Rig);
		return false;
	}
	Early->AnswerNow();
	TestTrue(TEXT("an Outer stage's answer is delivered"), Rig.Wait(Accepted) && Accepted->SuccessCalls.load() == 1);
	TestEqual(TEXT("... with no park line"), LinesOf(AcceptedHandle.GetId(), TEXT("auth wait")), 0);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	Rig.Drain();

	// A replay's park: its 401 line, and nothing for the park's end
	AcceptTokens(Rig, {});
	auto Replay = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle ReplayHandle = Observe(Rig.Client.Get(TEXT("/replay")), Replay).Send();
	if (!TestTrue(TEXT("its 401 parked it for a replay"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	ReplayHandle.Cancel();
	TestTrue(TEXT("Cancel() ends the replay's park"), Rig.Wait(Replay) && Replay->Error.Kind == EVaRestErrorKind::Canceled);
	TestEqual(TEXT("... the 401 line"), LinesOf(ReplayHandle.GetId(), TEXT("/replay -> 401")), 1);
	TestEqual(TEXT("... and no line for the park (a Gap, as a backoff)"), LinesOf(ReplayHandle.GetId(), TEXT("auth wait")) + LinesOf(ReplayHandle.GetId(), TEXT("-> canceled")), 0);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	Rig.Drain();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthNewerCredentialsTest, "VaRest.Http.Auth.NewerCredentialsReplayAtOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthNewerCredentialsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Concurrency review M-6, Task 17 review I-2: the game logs in again while an attempt is out with the old token. Its 401
	// then replays at once with the session's newer credentials (CredentialGeneration moved past the attempt's), starting
	// no refresh: a new attempt with GapBefore SessionRefresh, 0 s, and no refresh to name
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		const FString Given = Request.Headers.Get(TEXT("Authorization"));
		return Given == TEXT("Bearer old-access-123456") ? FVaRestFakeResponse::Manual() : Given == TEXT("Bearer new-access-123456") ? Json(200) : Json(401);
	});
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/a")), Outcome).Send();
	if (!TestTrue(TEXT("attempt 1 is out with the old token"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Client.SetBearerToken(TEXT("new-access-123456"));
	Rig.Fake->Complete(0, Json(401));
	if (!TestTrue(TEXT("its 401 replays with the new token, and succeeds"), Rig.Wait(Outcome) && Outcome->SuccessCalls.load() == 1))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("no refresh; two attempts"), Auth->Refreshes == 0 && !Auth->IsPending() && Outcome->Response.Attempts == 2 && Rig.Fake->NumCalls() == 2);
	const TArray<FVaRestTraceAttemptDispatched> Dispatched = DispatchesOf(*Scoped.Recorder, Handle.GetId());
	TestTrue(TEXT("IN-2: the replay's GapBefore is SessionRefresh, 0 s, no refresh id, no AuthWait"), Dispatched.Num() == 2
		&& Dispatched[1].GapKind == EVaRestTraceGap::SessionRefresh && Dispatched[1].GapSeconds == 0.0 && Dispatched[1].AuthRefreshId == 0
		&& Dispatched[1].AuthWait == 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthSameTokenWriteTest, "VaRest.Http.Auth.SameTokenWriteIsNoNewCredential",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthSameTokenWriteTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// M4a final review B3 (review B m-1, the concurrency review M-6 class through another door): CredentialGeneration counts
	// access-token values. A write that keeps the access token -- SetSession with only a new expiry, SetBearerToken of the
	// same token, an ImportSession of the live session -- is no new credential. So the generation stays; a 401 for an
	// attempt that carried that token refreshes and replays with the refreshed one, instead of replaying the refused token
	// at once and failing 401; and the same write while a refresh runs does not end it, so a parked request replays with
	// that refresh's token rather than the refused one.
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	const uint32 Before = Rig.Client.GetSession().CredentialGeneration;
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 3600.0);
	TestEqual(TEXT("SetSession with a new expiry only: no new credential"), Rig.Client.GetSession().CredentialGeneration, Before);
	Rig.Client.SetBearerToken(TEXT("old-access-123456"));
	TestEqual(TEXT("SetBearerToken of the same token (its expiry now unknown): no new credential"), Rig.Client.GetSession().CredentialGeneration, Before);
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 3600.0);
	TestTrue(TEXT("ImportSession of the live session (with its expiry): imported, no new credential"), Rig.Client.ImportSession(Rig.Client.ExportSession())
		&& Rig.Client.GetSession().CredentialGeneration == Before);

	// A held attempt with the old token; the same token written again; then its 401
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	auto Held = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), Held).Send();
	if (!TestTrue(TEXT("attempt 1 is out with the old token"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 1800.0);
	Rig.Fake->Complete(0, Json(401));
	PumpUntil([Auth, Held] { return Auth->IsPending() || Held->IsDone(); });
	TestTrue(TEXT("its 401 starts a refresh; the refused token is not replayed"), Auth->IsPending() && Auth->Refreshes == 1 && Rig.Fake->NumCalls() == 1);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	TestTrue(TEXT("... the replay carries the refreshed token, and succeeds"), Rig.Wait(Held) && Held->SuccessCalls.load() == 1 && Held->Response.Attempts == 2);
	Rig.Drain();

	// A request parked behind a refresh; the same token written again while it runs
	FTestRig Parked;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Hand = HandRig(Parked);
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Parked.Client);
	auto Waiting = MakeOutcome<FVaRestJson>();
	Observe(Parked.Client.Get(TEXT("/b")), Waiting).Send();
	if (!TestTrue(TEXT("its 401 parks it behind a refresh"), PumpUntil([Hand] { return Hand->IsPending(); }) && State.IsValid()))
	{
		Abandon(Parked);
		return false;
	}
	Parked.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 3600.0);
	TestTrue(TEXT("the same token written again does not end the refresh"), State->IsRefreshing() && !Waiting->IsDone());
	Hand->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	TestTrue(TEXT("... the parked request replays with the refresh's token, and succeeds"), Parked.Wait(Waiting) && Waiting->SuccessCalls.load() == 1
		&& Waiting->Response.Attempts == 2);
	Parked.Drain();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshTokenOnlyTest, "VaRest.Http.Auth.RefreshTokenOnlyParksAgain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshTokenOnlyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// M4a final review B4 (review B m-2): while a refresh runs, the game restores a refresh token only (SetSession("", R2),
	// the case NeedsRefresh's no-access-token rule exists for). That ends the refresh -- it is the old token's -- and its
	// parked requests park again behind a refresh from R2 instead of going out with no credentials (NoCredentials,
	// Kind=Auth): A's replay and B's first attempt, each still the same attempt (A3's numbering), each wait measured from
	// its first park. The same refresh token with only the access token dropped keeps the refresh running.
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto Replay = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle ReplayHandle = Observe(Rig.Client.Get(TEXT("/a")), Replay).Send();
	if (!TestTrue(TEXT("A's 401 parks it behind a refresh"), PumpUntil([Auth] { return Auth->IsPending(); }) && Rig.Fake->NumCalls() == 1))
	{
		Abandon(Rig);
		return false;
	}
	auto First = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle FirstHandle = Observe(Rig.Client.Get(TEXT("/b")), First).Send();
	Rig.Clock->Advance(0.25);
	Rig.Client.SetSession(TEXT(""), TEXT("game-refresh-123456"));
	if (!TestTrue(TEXT("the game's refresh token ends that refresh; both park behind a new one from it"), PumpUntil([Auth] { return Auth->Refreshes == 2; })
		&& Auth->IsPending() && Auth->LastRefreshToken.Equals(TEXT("game-refresh-123456"), ESearchCase::CaseSensitive)))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... nothing went out with no credentials"), !Replay->IsDone() && !First->IsDone() && Rig.Fake->NumCalls() == 1);
	const uint64 SecondRefresh = Auth->LastRefreshId;
	Rig.Clock->Advance(0.25);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	TestTrue(TEXT("A replays with the new token and succeeds (2 attempts)"), Rig.Wait(Replay) && Replay->SuccessCalls.load() == 1 && Replay->Response.Attempts == 2);
	TestTrue(TEXT("B goes out with it and succeeds (1 attempt)"), Rig.Wait(First) && First->SuccessCalls.load() == 1 && First->Response.Attempts == 1);
	Rig.Drain();
	const TArray<FVaRestTraceAttemptDispatched> Replayed = DispatchesOf(*Scoped.Recorder, ReplayHandle.GetId());
	TestTrue(TEXT("IN-2: A's replay is attempt 2, its Gap the whole 0.5 s, naming the second refresh"), Replayed.Num() == 2 && Replayed[1].Attempt == 2
		&& Replayed[1].GapKind == EVaRestTraceGap::SessionRefresh && FMath::IsNearlyEqual(Replayed[1].GapSeconds, 0.5) && Replayed[1].AuthRefreshId == SecondRefresh);
	const TArray<FVaRestTraceAttemptDispatched> Sent = DispatchesOf(*Scoped.Recorder, FirstHandle.GetId());
	TestTrue(TEXT("IN-2: B's is attempt 1, its AuthWait the whole 0.5 s, naming the second refresh"), Sent.Num() == 1 && Sent[0].Attempt == 1
		&& FMath::IsNearlyEqual(Sent[0].AuthWait, 0.5) && Sent[0].AuthRefreshId == SecondRefresh);
	FString Why;
	TestTrue(TEXT("PhasesHold for both"), PhasesHold(*Scoped.Recorder, ReplayHandle.GetId(), Why) && PhasesHold(*Scoped.Recorder, FirstHandle.GetId(), Why));

	// The same refresh token, the access token dropped: the refresh in flight is already one from that token, and goes on
	FTestRig Kept;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Hand = HandRig(Kept);
	auto Waiting = MakeOutcome<FVaRestJson>();
	Observe(Kept.Client.Get(TEXT("/c")), Waiting).Send();
	if (!TestTrue(TEXT("C's 401 parks it behind a refresh"), PumpUntil([Hand] { return Hand->IsPending(); })))
	{
		Abandon(Kept);
		return false;
	}
	Kept.Client.SetSession(TEXT(""), TEXT("old-refresh-123456"));
	Kept.Drain();
	TestTrue(TEXT("the access token dropped, the refresh token the same: still the one refresh"), Hand->Refreshes == 1 && Hand->IsPending() && !Waiting->IsDone());
	Hand->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	TestTrue(TEXT("... C replays with its token and succeeds"), Kept.Wait(Waiting) && Waiting->SuccessCalls.load() == 1 && Hand->Refreshes == 1);
	Kept.Drain();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRotatedRefreshTest, "VaRest.Http.Auth.RotatedRefreshTokenKeepsWaiters",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRotatedRefreshTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	for (bool bStaleSuccess : {false, true})
	{
		FTestRig Rig;
		const auto Auth = HandRig(Rig);
		FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
		const auto Expiries = MakeShared<int32, ESPMode::ThreadSafe>(0);
		Rig.Client.OnSessionExpired().AddLambda([Expiries] { ++*Expiries; });
		auto Replay = MakeOutcome<FVaRestJson>();
		const auto ReplayHandle = Observe(Rig.Client.Get(TEXT("/rotate-a")), Replay).Send();
		if (!TestTrue(TEXT("401 starts the original refresh"), PumpUntil([Auth] { return Auth->IsPending(); }) && Rig.Fake->NumCalls() == 1))
		{ Rig.Client.ClearSession(); Abandon(Rig); return false; }
		const uint64 OldId = Auth->LastRefreshId;
		auto OldAnswer = Auth->TakePending();
		auto First = MakeOutcome<FVaRestJson>();
		const auto FirstHandle = Observe(Rig.Client.Get(TEXT("/rotate-b")), First).Send();
		const auto External = MakeAnswers();
		Rig.Client.RefreshAuth(Record(External)); Rig.Client.RefreshAuth(Record(External));
		const uint32 Credentials = Rig.Client.GetSession().CredentialGeneration;
		Rig.Clock->Advance(.25);
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("game-refresh-123456"), 0, true);
		Rig.Client.WithAuth(FVaRestAuth::Bearer()); // the replacement still belongs to the original, independently owned provider
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("latest-refresh-123456"), 0, true);
		TestEqual(TEXT("rotation invokes no provider inside SetSession"), Auth->Refreshes, 1);
		TestEqual(TEXT("rotation does not complete external waiters inline"), External->Num(), 0);
		TestEqual(TEXT("same access token does not gain credential replay authority"), Rig.Client.GetSession().CredentialGeneration, Credentials);
		// Both stale rejection and stale success must be ignored, including an answer posted before the new provider starts.
		OldAnswer(bStaleSuccess ? FVaRestAuthRefreshResult::Refreshed(TEXT("late-access-123456"))
			: FVaRestAuthRefreshResult::Rejected(TEXT("old refresh token rejected")));
		if (!TestTrue(TEXT("one deferred replacement uses the latest token"), PumpUntil([Auth] { return Auth->Refreshes == 2 && Auth->IsPending(); })
			&& Auth->LastRefreshId != OldId && Auth->LastRefreshToken.Equals(TEXT("latest-refresh-123456"), ESearchCase::CaseSensitive)))
		{ Rig.Client.ClearSession(); Abandon(Rig); return false; }
		const uint64 NewId = Auth->LastRefreshId;
		TestTrue(TEXT("all waiters remain parked without replaying refused credentials"), !Replay->IsDone() && !First->IsDone()
			&& External->IsEmpty() && Rig.Fake->NumCalls() == 1);
		Rig.Clock->Advance(.25);
		Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
		const bool Completed = Rig.Wait(Replay) && Rig.Wait(First) && PumpUntil([External] { return External->Num() == 2; });
		if (!TestTrue(TEXT("request and external waiters complete behind the replacement"), Completed))
		{ Rig.Client.ClearSession(); Abandon(Rig); return false; }
		TestTrue(TEXT("reactive replay and first attempt preserve their numbering"), Replay->SuccessCalls.load() == 1 && Replay->Response.Attempts == 2
			&& First->SuccessCalls.load() == 1 && First->Response.Attempts == 1);
		TestTrue(TEXT("external waiters each receive the real refresh success"), (*External)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Refreshed, EVaRestErrorKind::None)
			&& (*External)[1] == (*External)[0]);
		Rig.Drain();
		TestEqual(TEXT("superseded replies never expire the replacement session"), *Expiries, 0);
		TestTrue(TEXT("current tokens survive stale replies"), Rig.Client.GetSession().AccessToken.Equals(TEXT("new-access-123456"), ESearchCase::CaseSensitive)
			&& Rig.Client.GetSession().RefreshToken.Equals(TEXT("latest-refresh-123456"), ESearchCase::CaseSensitive));
		TestEqual(TEXT("coalesced writes start one replacement"), Auth->Refreshes, 2);
		TestEqual(TEXT("one original attempt and two post-refresh sends"), Rig.Fake->NumCalls(), 3);
		const auto Replayed = DispatchesOf(*Scoped.Recorder, ReplayHandle.GetId());
		const auto Sent = DispatchesOf(*Scoped.Recorder, FirstHandle.GetId());
		TestTrue(TEXT("both dispatches name the replacement refresh"), Replayed.Num() == 2 && Replayed[1].AuthRefreshId == NewId
			&& Sent.Num() == 1 && Sent[0].AuthRefreshId == NewId);
		for (uint64 RequestId : {ReplayHandle.GetId(), FirstHandle.GetId()})
		{
			const auto Events = Scoped.Recorder->For(RequestId);
			const auto* Done = Events.FindByPredicate([](const FVaRestTraceEvent& Event) { return Event.Kind == EVaRestTraceKind::Completed; });
			TestTrue(TEXT("Completed also names the replacement refresh"), Done && Done->Payload.Get<FVaRestTraceCompleted>().AuthRefreshId == NewId);
		}
		FString Why;
		TestTrue(TEXT("phase accounting stays valid"), PhasesHold(*Scoped.Recorder, ReplayHandle.GetId(), Why) && PhasesHold(*Scoped.Recorder, FirstHandle.GetId(), Why));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRotatedRetirementTest, "VaRest.Http.Auth.RotatedRefreshRetirement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRotatedRetirementTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	for (bool bLogout : {false, true})
	{
		FTestRig Rig;
		const auto Auth = HandRig(Rig);
		const auto Replies = MakeAnswers();
		Rig.Client.RefreshAuth(Record(Replies));
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("replacement-refresh-123456"), 0, true);
		if (bLogout) Rig.Client.ClearSession();
		else Rig.Client.SetBearerToken(TEXT("game-access-123456"), true);
		TestEqual(TEXT("retirement delivery stays asynchronous"), Replies->Num(), 0);
		if (!TestTrue(TEXT("the original waiters still resolve"), PumpUntil([Replies] { return Replies->Num() == 1; })))
		{ Rig.Client.ClearSession(); Abandon(Rig); return false; }
		TestEqual(TEXT("logout or game credentials cancel the scheduled replacement"), Auth->Refreshes, 1);
		TestEqual(TEXT("the waiter sees the superseding game operation"), (*Replies)[0].Key,
			bLogout ? EVaRestAuthRefreshOutcome::Rejected : EVaRestAuthRefreshOutcome::Refreshed);
		Auth->Answer(FVaRestAuthRefreshResult::Rejected(TEXT("stale answer after retirement")));
		Rig.Drain();
		TestEqual(TEXT("retired refresh does not deliver twice"), Replies->Num(), 1);
		TestTrue(TEXT("stale answer preserves the game-selected session"), bLogout ? !Rig.Client.GetSession().HasAccessToken()
			: Rig.Client.GetSession().AccessToken.Equals(TEXT("game-access-123456"), ESearchCase::CaseSensitive));
		TestEqual(TEXT("no transport request"), Rig.Fake->NumCalls(), 0);
	}
	// A provider can own its pending Done. The coordinator must not keep that provider alive after configuration replaces it.
	FTestRig Rig;
	TSharedPtr<FHandAuth, ESPMode::ThreadSafe> Provider = HandRig(Rig);
	const TWeakPtr<FHandAuth, ESPMode::ThreadSafe> Weak = Provider;
	const auto Replies = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Replies));
	Rig.Client.WithAuth(FVaRestAuth::Bearer()); Provider.Reset();
	TestFalse(TEXT("an active refresh does not retain the removed provider"), Weak.IsValid());
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("replacement-refresh-123456"), 0, true);
	if (!TestTrue(TEXT("missing original provider resolves its waiters"), PumpUntil([Replies] { return Replies->Num() == 1; })))
	{ Rig.Client.ClearSession(); Abandon(Rig); return false; }
	TestTrue(TEXT("missing provider is a transient failure, not a logout"), (*Replies)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Failed, EVaRestErrorKind::Connection)
		&& Rig.Client.GetSession().RefreshToken.Equals(TEXT("replacement-refresh-123456"), ESearchCase::CaseSensitive));
	Rig.Drain();
	TestEqual(TEXT("old dropped Done and replacement failure complete once"), Replies->Num(), 1);
	TestEqual(TEXT("missing provider sends nothing"), Rig.Fake->NumCalls(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthProactiveFailedTest, "VaRest.Http.Auth.ProactiveFailedSendsAValidToken",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthProactiveFailedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// M4a final review B6 (ruling): a proactive refresh is an optimization. A request parked before its attempt by one
	// (a known expiry within the lead) is sent with its token when the refresh fails and that token has not expired yet; it
	// fails with the refresh's kind when the token has expired, and Kind=Auth when the refresh is rejected. Spec 4.2's
	// rule stays for the 401 cases: a request parked before its attempt behind a 401's refresh, and a replay parked behind a
	// proactive one, fail with the refresh's kind.
	const auto Due = [](FTestRig& Into)
	{
		const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Hand = HandRig(Into);
		Into.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 20.0);
		AcceptTokens(Into, { TEXT("old-access-123456"), TEXT("new-access-123456") });
		return Hand;
	};
	{
		FTestRig Rig;
		const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = Due(Rig);
		FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
		auto Outcome = MakeOutcome<FVaRestJson>();
		const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/valid")), Outcome).Send();
		if (!TestTrue(TEXT("valid: parked before its attempt, a proactive refresh"), Rig.Drain() && Auth->IsPending() && Rig.Fake->NumCalls() == 0))
		{
			Abandon(Rig);
			return false;
		}
		Rig.Clock->Advance(1.0);
		Auth->Answer(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Connection, TEXT("no route to the refresh endpoint")));
		TestTrue(TEXT("valid: Failed, the token 19 s from expiry -- sent with it, and succeeds"), Rig.Wait(Outcome) && Outcome->SuccessCalls.load() == 1
			&& Outcome->Response.Attempts == 1 && Rig.Fake->NumCalls() == 1);
		TestTrue(TEXT("... with the old token"), Rig.Fake->NumCalls() == 1
			&& Rig.Fake->GetCall(0).Request.Headers.Get(TEXT("Authorization")).Equals(TEXT("Bearer old-access-123456"), ESearchCase::CaseSensitive));
		Rig.Drain();
		const TArray<FVaRestTraceAttemptDispatched> Dispatched = DispatchesOf(*Scoped.Recorder, Handle.GetId());
		TestTrue(TEXT("... IN-2: attempt 1, the 1 s behind the refresh its AuthWait, naming that refresh"), Dispatched.Num() == 1 && Dispatched[0].Attempt == 1
			&& FMath::IsNearlyEqual(Dispatched[0].AuthWait, 1.0) && Dispatched[0].AuthRefreshId == Auth->LastRefreshId);
		FString Why;
		TestTrue(TEXT("... PhasesHold"), PhasesHold(*Scoped.Recorder, Handle.GetId(), Why));
		TestTrue(TEXT("... the session kept"), Rig.Client.GetSession().HasRefreshToken());
	}
	{
		FTestRig Rig;
		const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = Due(Rig);
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/expired")), Outcome).Send();
		Rig.Drain();
		Rig.Clock->Advance(25.0);
		Auth->Answer(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Timeout, TEXT("the refresh timed out")));
		TestTrue(TEXT("expired: Failed after the token expired -- fails with its kind (Timeout), nothing sent"), Rig.Wait(Outcome, 2.0)
			&& Outcome->Error.Kind == EVaRestErrorKind::Timeout && Rig.Fake->NumCalls() == 0);
		Rig.Drain();
	}
	{
		FTestRig Rig;
		const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = Due(Rig);
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/rejected")), Outcome).Send();
		Rig.Drain();
		Auth->Answer(FVaRestAuthRefreshResult::Rejected(TEXT("the refresh endpoint answered 400")));
		TestTrue(TEXT("rejected: Kind Auth, nothing sent"), Rig.Wait(Outcome, 2.0) && Outcome->Error.Kind == EVaRestErrorKind::Auth && Rig.Fake->NumCalls() == 0);
		Rig.Drain();
	}
	{
		// A's 401 starts the refresh; B starts while it runs and parks before its attempt, its token unexpired (no expiry)
		FTestRig Rig;
		const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
		auto Refused = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/a")), Refused).Send();
		if (!TestTrue(TEXT("reactive: A's 401 starts a refresh"), PumpUntil([Auth] { return Auth->IsPending(); })))
		{
			Abandon(Rig);
			return false;
		}
		auto Behind = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/b")), Behind).Send();
		Auth->Answer(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Connection, TEXT("no route to the refresh endpoint")));
		const bool bBoth = Rig.Wait(Refused, 2.0) && Rig.Wait(Behind, 2.0);
		TestTrue(TEXT("reactive: A and B fail Connection; B was not sent"), bBoth && Refused->Error.Kind == EVaRestErrorKind::Connection
			&& Behind->Error.Kind == EVaRestErrorKind::Connection && Rig.Fake->NumCalls() == 1);
		if (!bBoth)
		{
			Abandon(Rig);
		}
		Rig.Drain();
	}
	{
		// C is out with the token when its expiry comes due; D parks and starts a proactive refresh; C's 401 parks it for a
		// replay behind that refresh
		FTestRig Rig;
		const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
		Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
		{
			if (Request.Headers.Get(TEXT("Authorization")).Equals(TEXT("Bearer new-access-123456"), ESearchCase::CaseSensitive))
			{
				return Json(200);
			}
			return Request.Url.EndsWith(TEXT("/c")) ? Json(401) : Json(200);
		});
		Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
		auto Replay = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/c")), Replay).Send();
		if (!TestTrue(TEXT("replay: C is out"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
		{
			Abandon(Rig);
			return false;
		}
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 20.0);
		auto Proactive = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/d")), Proactive).Send();
		Rig.Fake->Complete(0, Json(401));
		if (!TestTrue(TEXT("replay: D's proactive refresh runs; C's 401 parks C behind it"), PumpUntil([Auth] { return Auth->IsPending(); })
			&& Rig.Drain() && Auth->Refreshes == 1 && !Replay->IsDone() && !Proactive->IsDone()))
		{
			Abandon(Rig);
			return false;
		}
		Auth->Answer(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Connection, TEXT("no route to the refresh endpoint")));
		const bool bBoth = Rig.Wait(Replay, 2.0) && Rig.Wait(Proactive, 2.0);
		TestTrue(TEXT("replay: D is sent and succeeds; C, a replay, fails Connection"), bBoth && Proactive->SuccessCalls.load() == 1
			&& Replay->Error.Kind == EVaRestErrorKind::Connection && Rig.Fake->NumCalls() == 2);
		if (!bBoth)
		{
			Abandon(Rig);
		}
		Rig.Drain();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthDeadline401Test, "VaRest.Http.Auth.DeadlinePassed401IsTheAnswer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthDeadline401Test::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 17 review M-3 (ruled): the gate follows DecideRetry. A 401 that comes back at or past the retry deadline is the
	// answer -- no attempt may start then (plan decision 10) -- so nothing parks, no refresh starts, nothing replays.
	// Timeout (Deadline) stays for a park the deadline cuts (VaRest.Http.Auth.ParkEndsAtItsDeadline).
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	const TSharedRef<FHeldAttempt, ESPMode::ThreadSafe> Held = MakeShared<FHeldAttempt, ESPMode::ThreadSafe>();
	Rig.Client.Use(Held, EVaRestStageSlot::PerAttempt);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")).Retry(FVaRestRetryPolicy::Disabled().WithDeadline(1.f)), Outcome).Send();
	if (!TestTrue(TEXT("the attempt is out (held: no transport timeout)"), PumpUntil([Held] { return Held->IsHolding(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(1.5);
	Rig.Drain();
	Held->Answer(401);
	TestEqual(TEXT("a 401 past the deadline starts no refresh"), Auth->Refreshes, 0);
	TestTrue(TEXT("... it is the answer: Kind Http, 401, one attempt"), Rig.Wait(Outcome) && Outcome->Error.Kind == EVaRestErrorKind::Http
		&& Outcome->Error.HttpStatus == 401 && Outcome->Response.Attempts == 1);
	Rig.Drain();
	TestTrue(TEXT("... once, and nothing parked"), Outcome->ErrorCalls.load() == 1 && Outcome->CompleteCalls.load() == 1 && !Auth->IsPending());
	if (!Outcome->IsDone())
	{
		// Drains what a park would have left (restore-the-bug only): the park's 0 s deadline timer fires on the next advance
		Rig.Clock->Advance(0.1);
		Rig.Wait(Outcome);
	}
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	Rig.Drain();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshMessageTest, "VaRest.Http.Auth.RefreshMessageScrubbed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshMessageTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 17 review M-5, Task 14 review M-1's rule: the refresh's message is the auth's own text, and a parked request's
	// FVaRestError::Message carries it. It holds no credential -- scrubbed by the request's redactor, which still knows the
	// tokens the rejection itself retired -- and is escaped, so a CR LF in it cannot forge a line in a game's log.
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	auto Rejected = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), Rejected).Send();
	if (!TestTrue(TEXT("parked"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Answer(FVaRestAuthRefreshResult::Rejected(TEXT("refused old-refresh-123456\r\nX-Forged: 1")));
	if (!TestTrue(TEXT("Rejected: Kind Auth"), Rig.Wait(Rejected) && Rejected->Error.Kind == EVaRestErrorKind::Auth))
	{
		Abandon(Rig);
		return false;
	}
	const FString& RejectedText = Rejected->Error.Message;
	TestTrue(TEXT("the token is scrubbed"), !RejectedText.Contains(TEXT("old-refresh-123456")) && RejectedText.Contains(TEXT("refused ***")));
	TestTrue(TEXT("... and the line break escaped"), !RejectedText.Contains(TEXT("\n")) && !RejectedText.Contains(TEXT("\r")) && RejectedText.Contains(TEXT("\\r\\nX-Forged: 1")));

	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	auto Failed = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/b")), Failed).Send();
	if (!TestTrue(TEXT("parked again"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Answer(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Connection, TEXT("no answer for old-access-123456\n")));
	TestTrue(TEXT("Failed: Kind Connection, the token scrubbed, the line break escaped"), Rig.Wait(Failed) && Failed->Error.Kind == EVaRestErrorKind::Connection
		&& !Failed->Error.Message.Contains(TEXT("old-access-123456")) && Failed->Error.Message.Contains(TEXT("no answer for ***\\n"))
		&& !Failed->Error.Message.Contains(TEXT("\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshJoinsTest, "VaRest.Http.Auth.RefreshAuthJoinsInFlight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshJoinsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 17 review M-8 (ruled): RefreshAuth waits for a refresh in flight even when the current auth cannot refresh --
	// none at all, or one that cannot (an auth change does not end the refresh, Task 16 review M-5): the refresh exists,
	// and its end applies to the session. With no refresh in flight it answers Unavailable (M4a final review B-I-1; it was
	// (Failed, Config) before that ruling).
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
	const FRefreshAnswers Started = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Started));
	if (!TestTrue(TEXT("a refresh in flight"), State.IsValid() && State->IsRefreshing() && Auth->Refreshes == 1))
	{
		return false;
	}
	Rig.Client.WithoutAuth();
	const FRefreshAnswers NoAuth = MakeAnswers();
	Rig.Client.RefreshAuth(Record(NoAuth));
	Rig.Client.WithAuth(FVaRestAuth::Bearer());
	const FRefreshAnswers Cannot = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Cannot));
	TestTrue(TEXT("both joined it: three waiters, still one refresh"), State->NumRefreshWaitersInternal() == 3 && Auth->Refreshes == 1);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	const FRefreshAnswer Refreshed(EVaRestAuthRefreshOutcome::Refreshed, EVaRestErrorKind::None);
	TestTrue(TEXT("all three told Refreshed"), PumpUntil([Started, NoAuth, Cannot] { return Started->Num() == 1 && NoAuth->Num() == 1 && Cannot->Num() == 1; })
		&& (*Started)[0] == Refreshed && (*NoAuth)[0] == Refreshed && (*Cannot)[0] == Refreshed);
	TestEqualSensitive(TEXT("... and the refresh applied to the session"), Rig.Client.GetSession().AccessToken, FString(TEXT("new-access-123456")));
	const FRefreshAnswers Refused = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Refused));
	TestTrue(TEXT("nothing in flight, an auth that cannot refresh: (Unavailable, None)"), PumpUntil([Refused] { return Refused->Num() == 1; })
		&& (*Refused)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Unavailable, EVaRestErrorKind::None));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthBearerRefreshTest, "VaRest.Http.Auth.BearerRefresh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthBearerRefreshTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	RefreshServer(Rig, NewTokens());
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	TSharedRef<TArray<EVaRestSessionChange>, ESPMode::ThreadSafe> Seen = MakeShared<TArray<EVaRestSessionChange>, ESPMode::ThreadSafe>();
	Rig.Client.OnSessionChanged().AddLambda([Seen](EVaRestSessionChange Change) { Seen->Add(Change); });
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/protected")), Outcome).Send();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)) || !TestEqual(TEXT("401, the refresh, the replay"), Rig.Fake->NumCalls(), 3))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("a success on the replay"), Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 2);
	const FVaRestTransportRequest& Refresh = Rig.Fake->GetCall(1).Request;
	TestTrue(TEXT("the refresh: POST to the refresh path"), Refresh.Verb == TEXT("POST") && Refresh.Url == TEXT("https://api.test/auth/refresh"));
	TestEqualSensitive(TEXT("... a JSON body with the refresh token under RefreshBodyField"), BodyText(Rig.Fake->ReadBody(1)), FString(TEXT(R"({"refreshToken":"old-refresh-123456"})")));
	TestTrue(TEXT("... application/json, no Authorization (it bypasses the auth stages)"), Refresh.Headers.Get(TEXT("Content-Type")) == TEXT("application/json")
		&& !Refresh.Headers.Contains(TEXT("Authorization")));
	TestTrue(TEXT("... Highest for the engine (it skips the limiter's queue, ruling M-8)"), Refresh.Priority == EVaRestPriority::Highest);
	const FVaRestSessionData Data = Rig.Client.GetSession();
	TestTrue(TEXT("the session: both new tokens, the expiry from expiresIn"), Data.AccessToken == TEXT("new-access-123456")
		&& Data.RefreshToken == TEXT("new-refresh-123456") && Data.ExpiresAt == 60.0);
	TestTrue(TEXT("OnSessionChanged(Refreshed), once"), Seen->Num() == 1 && (*Seen)[0] == EVaRestSessionChange::Refreshed);
	const uint64 RefreshId = Refresh.RequestId;
	bool bFlagged = false;
	for (const FVaRestTraceEvent& Event : Scoped.Recorder->For(RefreshId))
	{
		bFlagged |= Event.Kind == EVaRestTraceKind::Started && EnumHasAnyFlags(Event.Payload.Get<FVaRestTraceStarted>().Flags, EVaRestTraceRequestFlags::AuthRefresh);
	}
	TestTrue(TEXT("IN-1: the refresh request has flag AuthRefresh"), bFlagged);
	// Spec review I-2: the refresh request's own RequestId is the refresh's id, which the replay names (IN-2 AuthRefreshId)
	const TArray<FVaRestTraceAttemptDispatched> Dispatched = DispatchesOf(*Scoped.Recorder, Handle.GetId());
	TestTrue(TEXT("the replay names the refresh request by its id"), Dispatched.Num() == 2 && Dispatched[1].AuthRefreshId == RefreshId);
	TestTrue(TEXT("IN-9: SessionRefreshed, caused by the 401"), Scoped.Recorder->MarkersOf(Rig.Client.GetId()).ContainsByPredicate([&Handle](const FVaRestTraceMarker& Marker)
	{
		return Marker.Kind == EVaRestTraceMarkerKind::SessionRefreshed && Marker.CauseRequestId == Handle.GetId();
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthOAuthFormTest, "VaRest.Http.Auth.OAuth2Form",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthOAuthFormTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// OpenAPI N15 / 7.7: an oauth2 password or client-credentials flow refreshes with RFC 6749 6
	FVaRestBearerRefreshConfig Config;
	Config.RefreshPath = TEXT("/auth/token");
	Config.Style = EVaRestRefreshStyle::OAuth2Form;
	Config.OAuthClientId = TEXT("game");
	Config.AccessTokenPath = TEXT("access_token");
	Config.RefreshTokenPath = TEXT("refresh_token");
	Config.ExpiresInPath = TEXT("expires_in");
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(Config));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	RefreshServer(Rig, Json(200, R"({"access_token":"new-access-123456","token_type":"Bearer","expires_in":"30"})"));
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)) || !TestEqual(TEXT("three calls"), Rig.Fake->NumCalls(), 3))
	{
		Abandon(Rig);
		return false;
	}
	TestEqualSensitive(TEXT("the form body"), BodyText(Rig.Fake->ReadBody(1)), FString(TEXT("grant_type=refresh_token&refresh_token=old-refresh-123456&client_id=game")));
	TestEqualSensitive(TEXT("... form-encoded"), Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("Content-Type")), FString(TEXT("application/x-www-form-urlencoded")));
	const FVaRestSessionData Data = Rig.Client.GetSession();
	TestTrue(TEXT("a response without a refresh token keeps the old one; a numeric string is an expiry"), Data.AccessToken == TEXT("new-access-123456")
		&& Data.RefreshToken == TEXT("old-refresh-123456") && Data.ExpiresAt == 30.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthOAuthRfcNamesTest, "VaRest.Http.Auth.OAuth2FormReadsRfcNames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthOAuthRfcNamesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// M4a final review B2 (review B I-2): the path defaults are camelCase, but an OAuth 2 token endpoint answers RFC 6749
	// 5.1's access_token, refresh_token and expires_in. For OAuth2Form, a path that finds nothing is read again at the RFC
	// name, so a code-first OAuth2Form client with the defaults refreshes instead of logging the player out at its first
	// refresh; a camelCase server still works, a configured path that finds a value wins, the Json style reads only its
	// paths, and the two token names are the auth's redaction keys.
	struct FCase
	{
		const TCHAR* Name;
		EVaRestRefreshStyle Style;
		const char* Response;
		bool bRefreshed;
		const TCHAR* RefreshToken;
		double ExpiresAt;
	};
	const FCase Cases[] = {
		{ TEXT("OAuth2Form, default paths, an RFC response"), EVaRestRefreshStyle::OAuth2Form,
			R"({"access_token":"new-access-123456","token_type":"Bearer","refresh_token":"new-refresh-123456","expires_in":60})", true, TEXT("new-refresh-123456"), 60.0 },
		{ TEXT("OAuth2Form, default paths, a camelCase response"), EVaRestRefreshStyle::OAuth2Form,
			R"({"accessToken":"new-access-123456","refreshToken":"new-refresh-123456","expiresIn":60})", true, TEXT("new-refresh-123456"), 60.0 },
		{ TEXT("OAuth2Form: a path that finds a value wins over the RFC name"), EVaRestRefreshStyle::OAuth2Form,
			R"({"accessToken":"new-access-123456","access_token":"rfc-access-123456","expires_in":30})", true, TEXT("old-refresh-123456"), 30.0 },
		{ TEXT("Json style, an RFC response: no fallback, Rejected"), EVaRestRefreshStyle::Json,
			R"({"access_token":"new-access-123456","refresh_token":"new-refresh-123456"})", false, TEXT(""), 0.0 },
	};
	for (const FCase& Case : Cases)
	{
		FVaRestBearerRefreshConfig Config;
		Config.Style = Case.Style;
		FTestRig Rig;
		Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(Config));
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
		TSharedRef<int32, ESPMode::ThreadSafe> Expired = MakeShared<int32, ESPMode::ThreadSafe>(0);
		Rig.Client.OnSessionExpired().AddLambda([Expired] { ++*Expired; });
		RefreshServer(Rig, Json(200, Case.Response));
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
		if (!TestTrue(FString::Printf(TEXT("%s: completes"), Case.Name), Rig.Wait(Outcome) && Rig.Drain()))
		{
			Abandon(Rig);
			return false;
		}
		const FVaRestSessionData Data = Rig.Client.GetSession();
		if (Case.bRefreshed)
		{
			TestTrue(FString::Printf(TEXT("%s: Refreshed, the replay succeeds"), Case.Name), Outcome->SuccessCalls.load() == 1 && *Expired == 0);
			TestTrue(FString::Printf(TEXT("%s: the session holds the response's tokens and expiry"), Case.Name),
				Data.AccessToken.Equals(TEXT("new-access-123456"), ESearchCase::CaseSensitive) && Data.RefreshToken.Equals(Case.RefreshToken, ESearchCase::CaseSensitive)
				&& FMath::IsNearlyEqual(Data.ExpiresAt, Case.ExpiresAt));
		}
		else
		{
			TestTrue(FString::Printf(TEXT("%s: Kind Auth, the session expired"), Case.Name), Outcome->Error.Kind == EVaRestErrorKind::Auth && *Expired == 1
				&& !Data.HasAccessToken() && !Data.HasRefreshToken());
		}
	}

	FVaRestBearerRefreshConfig Form;
	Form.Style = EVaRestRefreshStyle::OAuth2Form;
	TArray<FString> FormKeys;
	FVaRestAuth::BearerRefresh(Form)->GetRedactedKeys(FormKeys);
	TestTrue(TEXT("OAuth2Form: access_token and refresh_token are the auth's redaction keys"), FormKeys.Contains(TEXT("access_token")) && FormKeys.Contains(TEXT("refresh_token")));
	TestFalse(TEXT("... expires_in is no secret"), FormKeys.Contains(TEXT("expires_in")));
	TArray<FString> JsonKeys;
	FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig())->GetRedactedKeys(JsonKeys);
	TestFalse(TEXT("the Json style reads no RFC name, so adds no key for one"), JsonKeys.Contains(TEXT("access_token")));
	FTestRig Keyed;
	Keyed.Client.WithAuth(FVaRestAuth::BearerRefresh(Form));
	const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Redactor = Keyed.Client.GetRedactor();
	TestTrue(TEXT("... and the client redacts both in a JSON body"), Redactor->GetRules().ShouldRedact(FStringView(TEXT("access_token")), EVaRestRedactKeyKind::Json)
		&& Redactor->GetRules().ShouldRedact(FStringView(TEXT("refresh_token")), EVaRestRedactKeyKind::Json));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshOutcomesTest, "VaRest.Http.Auth.RefreshOutcomes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshOutcomesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Ruling I-7: Rejected only for the endpoint's own "no"; everything else is Failed, the session kept
	struct FCase
	{
		const TCHAR* Name;
		FVaRestFakeResponse Answer;
		EVaRestErrorKind Kind;
		bool bRetryable;
		bool bExpires;
	};
	const FCase Cases[] = {
		{ TEXT("400: rejected"), Json(400, R"({"error":"invalid_grant"})"), EVaRestErrorKind::Auth, false, true },
		{ TEXT("503: failed"), Json(503), EVaRestErrorKind::Connection, true, false },
		{ TEXT("429: failed"), Json(429), EVaRestErrorKind::Connection, true, false },
		{ TEXT("408: failed"), Json(408), EVaRestErrorKind::Connection, true, false },
		{ TEXT("no access token: rejected"), Json(200, R"({"refreshToken":"x"})"), EVaRestErrorKind::Auth, false, true },
		{ TEXT("not JSON: rejected"), FVaRestFakeResponse::Text(200, TEXT("ok")), EVaRestErrorKind::Auth, false, true },
		{ TEXT("connection error: failed"), FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError")), EVaRestErrorKind::Connection, true, false },
		{ TEXT("timeout: failed as Timeout"), FVaRestFakeResponse::Failed(EVaRestTransportFailure::Timeout, TEXT("TimedOut")), EVaRestErrorKind::Timeout, true, false },
		// Concurrency review I-7's row: a local switch must not log the player out
		{ TEXT("HTTP disabled: session kept, the parked request Kind Config, not retryable"), FVaRestFakeResponse::Failed(EVaRestTransportFailure::Other, TEXT("HttpDisabled")),
			EVaRestErrorKind::Config, false, false },
	};
	for (const FCase& Case : Cases)
	{
		FTestRig Rig;
		Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
		RefreshServer(Rig, Case.Answer);
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
		if (!TestTrue(FString::Printf(TEXT("%s: completes"), Case.Name), Rig.Wait(Outcome)))
		{
			Abandon(Rig);
			return false;
		}
		TestEqual(FString::Printf(TEXT("%s: the parked request's kind"), Case.Name), Outcome->Error.Kind, Case.Kind);
		TestEqual(FString::Printf(TEXT("%s: ... retryable or not"), Case.Name), Outcome->Error.bRetryable, Case.bRetryable);
		TestEqual(FString::Printf(TEXT("%s: the session expired or kept (plan decision 3)"), Case.Name), !Rig.Client.GetSession().HasRefreshToken(), Case.bExpires);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthBearerProactiveTest, "VaRest.Http.Auth.BearerRefreshProactive",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthBearerProactiveTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 20.0);
	RefreshServer(Rig, NewTokens());
	// Concurrency review M-7: the lead is at most half the token's lifetime -- 10 s for this one, not the configured 30 s,
	// which would refresh before every request
	TestFalse(TEXT("a fresh 20 s token is not due at once"), Rig.Client.NeedsAuthRefresh());
	Rig.Clock->Advance(9.0);
	TestFalse(TEXT("... nor with 11 s left"), Rig.Client.NeedsAuthRefresh());
	Rig.Clock->Advance(1.0);
	TestTrue(TEXT("... it is with 10 s left"), Rig.Client.NeedsAuthRefresh());
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)) || !TestEqual(TEXT("the refresh, then one attempt"), Rig.Fake->NumCalls(), 2))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("spec 4.2: refreshed before the known expiry, no 401 needed"), Rig.Fake->GetCall(0).Request.Url.Contains(TEXT("/auth/refresh"))
		&& Outcome->Response.Attempts == 1 && Outcome->SuccessCalls.load() == 1);
	TestFalse(TEXT("the new 60 s token is not due"), Rig.Client.NeedsAuthRefresh());

	// Re-review N-10: an imported session's time left is not its lifetime (Task 13 records it as unknown), so the configured
	// 30 s lead applies -- not half of the time left -- and an expired import is due at once
	FTestRig Source;
	Source.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 40.0);
	FTestRig Imported;
	Imported.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	if (!TestTrue(TEXT("imported"), Imported.Client.ImportSession(Source.Client.ExportSession())))
	{
		return false;
	}
	TestFalse(TEXT("an import with about 40 s left is not due"), Imported.Client.NeedsAuthRefresh());
	// The expiry crosses UTC, so it is within a second of 40 s: 12 s later 27-29 s are left, inside the 30 s lead
	Imported.Clock->Advance(12.0);
	TestTrue(TEXT("... with 27-29 s left it is, by the configured lead"), Imported.Client.NeedsAuthRefresh());
	FTestRig Stale;
	Stale.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	TestTrue(TEXT("an expired session imports"), Stale.Client.ImportSession(TEXT(R"({"version":1,"accessToken":"stale-access-123456","refreshToken":"stale-refresh-123456","expiresAt":"2020-01-01T00:00:00.000Z"})")));
	TestTrue(TEXT("... and is due at once (the inline ruling on a past expiresAt)"), Stale.Client.NeedsAuthRefresh());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthBearerFlightTest, "VaRest.Http.Auth.BearerRefreshFailsOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthBearerFlightTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Spec review I-3 (Review Focus line 2) with the real refresh request: twenty 401s while the refresh endpoint is slow,
	// then the refresh fails on the network -- one refresh request, each parked request fails once, the session stays
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	RefreshServer(Rig, FVaRestFakeResponse::Manual());
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> Outcomes;
	for (int32 Index = 0; Index < 20; ++Index)
	{
		Outcomes.Add(MakeOutcome<FVaRestJson>());
		Observe(Rig.Client.Get(FString::Printf(TEXT("/p/%d"), Index)), Outcomes.Last()).Send();
	}
	if (!TestTrue(TEXT("twenty 401s and one refresh call"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 21; }) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	const int32 RefreshCall = FindCall(Rig, TEXT("/auth/refresh"));
	TestTrue(TEXT("exactly one call to the refresh endpoint"), RefreshCall != INDEX_NONE && FindCall(Rig, TEXT("/auth/refresh"), 1) == INDEX_NONE);
	if (RefreshCall == INDEX_NONE)
	{
		Abandon(Rig);
		return false;
	}
	Rig.Fake->Complete(RefreshCall, FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError")));
	const auto AllDone = [&Outcomes] { for (const auto& One : Outcomes) { if (!One->IsDone()) { return false; } } return true; };
	if (!TestTrue(TEXT("all complete"), PumpUntil(AllDone)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	bool bEachOnce = true;
	for (const auto& One : Outcomes)
	{
		bEachOnce &= One->ErrorCalls.load() == 1 && One->CompleteCalls.load() == 1 && One->Error.Kind == EVaRestErrorKind::Connection && One->Error.bRetryable;
	}
	TestTrue(TEXT("each fails once: Connection, retryable"), bEachOnce);
	TestEqual(TEXT("none replayed"), Rig.Fake->NumCalls(), 21);
	TestTrue(TEXT("the session is kept"), Rig.Client.GetSession().HasRefreshToken() && Rig.Client.GetSession().AccessToken == TEXT("old-access-123456"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRedactedTest, "VaRest.Http.Auth.RefreshedTokenRedacted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRedactedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Review Focus: the refreshed token echoed in a header, a text body and a Location URL; the old one in a 401 body
	TGuardConsoleVariable<int32> Level(TEXT("VaRest.LogLevel"), -1);
	FTestRig Rig;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Body).WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.Contains(TEXT("/auth/")))
		{
			return NewTokens();
		}
		if (Request.Url.EndsWith(TEXT("/b")))
		{
			// Spec review M-11: in flight across the refresh; the test answers it after the new token exists
			return FVaRestFakeResponse::Manual();
		}
		if (Request.Headers.Get(TEXT("Authorization")) != TEXT("Bearer new-access-123456"))
		{
			return Json(401, R"({"error":"expired","hint":"old-access-123456"})");
		}
		FVaRestFakeResponse Echo = FVaRestFakeResponse::Text(200, TEXT("your token is new-access-123456"));
		Echo.Headers.Set(TEXT("X-Echo"), TEXT("new-access-123456"));
		Echo.Headers.Set(TEXT("Location"), TEXT("/next?t=new-access-123456"));
		return Echo;
	});
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Headers);
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("-> 401"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	// Spec review M-11 (Review Focus line 4): /b goes out with the old token before the refresh and is answered after it
	auto InFlight = MakeOutcome<FString>();
	const FVaRestRequestHandle InFlightHandle = Observe(Rig.Client.Get<FString>(TEXT("/b")), InFlight).Send();
	if (!TestTrue(TEXT("/b is out, with the old token"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	auto Outcome = MakeOutcome<FString>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get<FString>(TEXT("/p")), Outcome).Send();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	FVaRestFakeResponse Late = FVaRestFakeResponse::Text(200, TEXT("refreshed to new-access-123456"));
	Late.Headers.Set(TEXT("X-Echo"), TEXT("new-access-123456"));
	Late.Headers.Set(TEXT("Location"), TEXT("/after?t=new-access-123456"));
	Rig.Fake->Complete(FindCall(Rig, TEXT("/b")), MoveTemp(Late));
	if (!TestTrue(TEXT("/b completes after the refresh"), Rig.Wait(InFlight)))
	{
		Abandon(Rig);
		return false;
	}
	FlushRedactionPipe();
	const TCHAR* Tokens[] = { TEXT("new-access-123456"), TEXT("old-access-123456"), TEXT("new-refresh-123456"), TEXT("old-refresh-123456") };
	for (const FString& Line : Capture.GetLines())
	{
		for (const TCHAR* Token : Tokens)
		{
			if (Line.Contains(Token))
			{
				AddError(FString::Printf(TEXT("a log line holds %s: %s"), Token, *Line));
			}
		}
	}
	TArray<FString> Traced;
	for (const uint64 Id : { Handle.GetId(), InFlightHandle.GetId() })
	{
		for (const FVaRestTraceEvent& Event : Scoped.Recorder->For(Id))
		{
			if (Event.Kind == EVaRestTraceKind::AttemptEnded)
			{
				const FVaRestTraceAttemptEnded& Ended = Event.Payload.Get<FVaRestTraceAttemptEnded>();
				Traced.Add(Ended.EffectiveUrl);
				for (const FVaRestHeaders::FPair& Pair : Ended.Headers)
				{
					Traced.Add(Pair.Value);
				}
			}
		}
	}
	for (const FString& Value : Traced)
	{
		for (const TCHAR* Token : Tokens)
		{
			if (Value.Contains(Token))
			{
				AddError(FString::Printf(TEXT("a trace field holds %s: %s"), Token, *Value));
			}
		}
	}
	TestTrue(TEXT("the echoed header is in the trace, scrubbed"), Traced.Contains(TEXT("***")));
	TestTrue(TEXT("the body line was written, scrubbed"), Capture.Contains(TEXT("your token is ***")));
	TestTrue(TEXT("... and the in-flight request's too (plan decision 17: the current redactor)"), Capture.Contains(TEXT("refreshed to ***")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthPathKeysTest, "VaRest.Http.Auth.TokenPathKeys",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthPathKeysTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Carry (d), M3 review M19: a token path's last segment is a key; "bearer" matches no built-in rule (VaRestJsonRedact.h)
	FVaRestBearerRefreshConfig Config;
	Config.AccessTokenPath = TEXT("data.auth.bearer");
	Config.RefreshTokenPath = TEXT("/data/renewal");
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(Config));
	// Held: the rules live in the redactor, which a later publish would replace
	const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Redactor = VaRest::Http::FClientAccess::State(Rig.Client)->GetRedactor();
	const FVaRestRedactRules& Rules = Redactor->GetRules();
	TestTrue(TEXT("the dotted path's last segment"), Rules.ShouldRedact(FStringView(TEXT("bearer")), EVaRestRedactKeyKind::Json));
	TestTrue(TEXT("the pointer's last segment"), Rules.ShouldRedact(FStringView(TEXT("renewal")), EVaRestRedactKeyKind::Json));
	TestFalse(TEXT("not the whole path"), Rules.ShouldRedact(FStringView(TEXT("data")), EVaRestRedactKeyKind::Json));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthBearerGateTest, "VaRest.Http.Auth.BearerRefreshPassesTheGate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthBearerGateTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 17 review M-7 (a): with two requests parked behind a 401-triggered refresh and both slots held, the real refresh
	// request passes the gate (parked there, it would wait for its own refresh: the bIsAuthRefresh pass-through,
	// VaRestAuthGate.cpp:378-379) and the full limiter (ruling M-8, VaRestPipeline.cpp:368-377) to the transport. The parked
	// requests then replay with the new token. Task 18 review M-5: a Send() on the game thread enters at once
	// (VaRestClient.cpp:210-214) and the chain runs synchronously from there (VaRestPipeline.cpp:919), so the refresh
	// request the first park sends would reach the transport before a second 401 could park; a PerAttempt stage below the
	// gate and the limiter holds it until the second has.
	FTestRig Rig;
	const TSharedRef<FRefreshHold, ESPMode::ThreadSafe> Hold = MakeShared<FRefreshHold, ESPMode::ThreadSafe>();
	Rig.Client.WithMaxConcurrent(2).WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig())).Use(Hold, EVaRestStageSlot::PerAttempt);
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	// Whatever is not sent with the new token -- the protected requests' first attempts, the long polls, the refresh
	// request -- waits for the test to answer it
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		return Request.Headers.Get(TEXT("Authorization")) == TEXT("Bearer new-access-123456") ? Json(200, R"({"ok":true})") : FVaRestFakeResponse::Manual();
	});
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
	if (!TestTrue(TEXT("a client state"), State.IsValid()))
	{
		return false;
	}
	VaRest::Http::FLimiter& Limiter = State->GetLimiter();
	// Every early end lets the held attempt go first, so the stage keeps no exchange (and through it the client) alive
	const auto Stop = [&Rig, &Hold]
	{
		Hold->Release();
		Abandon(Rig);
	};
	auto First = MakeOutcome<FVaRestJson>();
	auto Second = MakeOutcome<FVaRestJson>();
	auto PollA = MakeOutcome<FVaRestJson>();
	auto PollB = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/p/0")), First).Send();
	Observe(Rig.Client.Get(TEXT("/p/1")), Second).Send();
	Observe(Rig.Client.Get(TEXT("/poll/0")), PollA).Send();
	Observe(Rig.Client.Get(TEXT("/poll/1")), PollB).Send();
	if (!TestTrue(TEXT("the protected requests hold both slots, the polls queue behind them"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }) && Rig.Drain()
		&& FindCall(Rig, TEXT("/p/0")) != INDEX_NONE && FindCall(Rig, TEXT("/p/1")) != INDEX_NONE && Limiter.NumActive() == 2 && Limiter.NumQueued() == 2))
	{
		Stop();
		return false;
	}
	// The 401 frees its slot before the gate sees it (VaRestPipeline.cpp:407-411), and the release grants /poll/0 at once
	// (VaRestLimiter.cpp:58-61): /p/0 parks and the refresh starts with /p/1 and /poll/0 holding the two slots
	Rig.Fake->Complete(FindCall(Rig, TEXT("/p/0")), Json(401));
	if (!TestTrue(TEXT("the refresh request passed the gate and the full limiter: the stage below them holds it"), PumpUntil([&Hold] { return Hold->IsHolding(); }) && Rig.Drain()))
	{
		Stop();
		return false;
	}
	TestTrue(TEXT("... /p/1 and /poll/0 hold the slots, /poll/1 still queued: the refresh request took none (ruling M-8)"), Limiter.NumActive() == 2 && Limiter.NumQueued() == 1
		&& FindCall(Rig, TEXT("/poll/0")) != INDEX_NONE && FindCall(Rig, TEXT("/p/1")) != INDEX_NONE && !Rig.Fake->GetCall(FindCall(Rig, TEXT("/p/1"))).bCompleted);
	Rig.Fake->Complete(FindCall(Rig, TEXT("/p/1")), Json(401));
	if (!TestTrue(TEXT("/p/1's 401 parks it behind the same refresh, and /poll/1 takes its slot"),
		PumpUntil([&Rig, &State] { return State->NumRefreshWaitersInternal() == 2 && FindCall(Rig, TEXT("/poll/1")) != INDEX_NONE; }) && Rig.Drain()))
	{
		Stop();
		return false;
	}
	Hold->Release();
	if (!TestTrue(TEXT("the refresh request reaches the transport"), PumpUntil([&Rig] { return FindCall(Rig, TEXT("/auth/refresh")) != INDEX_NONE; }) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	const int32 RefreshCall = FindCall(Rig, TEXT("/auth/refresh"));
	TestTrue(TEXT("... with two requests parked and both slots held by the polls, none queued"), State->NumRefreshWaitersInternal() == 2
		&& Limiter.NumActive() == 2 && Limiter.NumQueued() == 0 && FindCall(Rig, TEXT("/auth/refresh"), 1) == INDEX_NONE);
	TestFalse(TEXT("... bare: no Authorization"), Rig.Fake->GetCall(RefreshCall).Request.Headers.Contains(TEXT("Authorization")));
	Rig.Fake->Complete(RefreshCall, NewTokens());
	// The replays are attempts like any other: they queue for a slot
	if (!TestTrue(TEXT("refreshed: both replays wait in the limiter's queue"), PumpUntil([&Rig, &Limiter]
		{
			return Rig.Client.GetSession().AccessToken == TEXT("new-access-123456") && Limiter.NumQueued() == 2;
		}) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	// Task 18 review M-4: the indexes checked before they are used
	const int32 PollCallA = FindCall(Rig, TEXT("/poll/0"));
	const int32 PollCallB = FindCall(Rig, TEXT("/poll/1"));
	if (!TestTrue(TEXT("both polls are out"), PollCallA != INDEX_NONE && PollCallB != INDEX_NONE))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Fake->Complete(PollCallA, Json(200));
	Rig.Fake->Complete(PollCallB, Json(200));
	const auto AllDone = [&First, &Second, &PollA, &PollB] { return First->IsDone() && Second->IsDone() && PollA->IsDone() && PollB->IsDone(); };
	if (!TestTrue(TEXT("all complete"), PumpUntil(AllDone) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("both parked requests succeed on their replay"), First->SuccessCalls.load() == 1 && First->Response.Attempts == 2
		&& Second->SuccessCalls.load() == 1 && Second->Response.Attempts == 2);
	TestTrue(TEXT("... the polls too"), PollA->SuccessCalls.load() == 1 && PollB->SuccessCalls.load() == 1);
	TestEqual(TEXT("seven calls: two 401s, two polls, one refresh, two replays"), Rig.Fake->NumCalls(), 7);
	const int32 FirstReplay = FindCall(Rig, TEXT("/p/0"), 1);
	const int32 SecondReplay = FindCall(Rig, TEXT("/p/1"), 1);
	TestTrue(TEXT("... the replays carry the new token"), FirstReplay != INDEX_NONE && SecondReplay != INDEX_NONE
		&& Rig.Fake->GetCall(FirstReplay).Request.Headers.Get(TEXT("Authorization")) == TEXT("Bearer new-access-123456")
		&& Rig.Fake->GetCall(SecondReplay).Request.Headers.Get(TEXT("Authorization")) == TEXT("Bearer new-access-123456"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthBearerTimeoutTest, "VaRest.Http.Auth.BearerRefreshTimesOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthBearerTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 17 review M-7 (b): a refresh request that times out -- the client's total timeout, which the refresh request has
	// as any request does (FVaRestAuth::BearerRefresh) -- reaches the parked requests as Kind=Timeout, retryable, and the
	// session stays (ruling I-7)
	{
		FTestRig Rig;
		Rig.Client.WithTimeout(5.f, 10.f).WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
		RefreshServer(Rig, FVaRestFakeResponse::Manual());
		const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
		auto First = MakeOutcome<FVaRestJson>();
		auto Second = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/p/a")), First).Send();
		Observe(Rig.Client.Get(TEXT("/p/b")), Second).Send();
		if (!TestTrue(TEXT("two 401s parked behind one refresh request"), State.IsValid()
			&& PumpUntil([&Rig, &State] { return FindCall(Rig, TEXT("/auth/refresh")) != INDEX_NONE && State->NumRefreshWaitersInternal() == 2; }) && Rig.Drain()))
		{
			Abandon(Rig);
			return false;
		}
		const int32 RefreshCall = FindCall(Rig, TEXT("/auth/refresh"));
		// The total below the activity timeout, so the total ends it (the two at one time would race, as in the engine)
		TestTrue(TEXT("the refresh request has the client's timeouts"), Rig.Fake->GetCall(RefreshCall).Request.Timeout == 5.f
			&& Rig.Fake->GetCall(RefreshCall).Request.ActivityTimeout == 10.f);
		Rig.Clock->Advance(5.0);
		if (!TestTrue(TEXT("both complete"), PumpUntil([&First, &Second] { return First->IsDone() && Second->IsDone(); }) && Rig.Drain()))
		{
			Abandon(Rig);
			return false;
		}
		TestTrue(TEXT("each fails once: Timeout, retryable"), First->ErrorCalls.load() == 1 && First->Error.Kind == EVaRestErrorKind::Timeout && First->Error.bRetryable
			&& Second->ErrorCalls.load() == 1 && Second->Error.Kind == EVaRestErrorKind::Timeout && Second->Error.bRetryable);
		TestEqual(TEXT("none replayed"), Rig.Fake->NumCalls(), 3);
		TestTrue(TEXT("the session is kept"), Rig.Client.GetSession().HasRefreshToken() && Rig.Client.GetSession().AccessToken == TEXT("old-access-123456"));
	}
	// Task 16 review note, carry 4: with no client timeout the refresh request has the engine's defaults
	// (HttpModule.cpp:96-98: no total, 30 s of activity), so a refresh endpoint that stops answering ends at the activity
	// timeout -- Connection (the fake ends it as the engine does, HttpRequestCommon.cpp:303-305), retryable, the session kept
	{
		FTestRig Rig;
		Rig.Fake->EngineActivityTimeout = 30.f;
		Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
		RefreshServer(Rig, FVaRestFakeResponse::Manual());
		auto Parked = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/p/c")), Parked).Send();
		if (!TestTrue(TEXT("idle: the refresh request is out"), PumpUntil([&Rig] { return FindCall(Rig, TEXT("/auth/refresh")) != INDEX_NONE; }) && Rig.Drain()))
		{
			Abandon(Rig);
			return false;
		}
		const FVaRestTransportRequest& Sent = Rig.Fake->GetCall(FindCall(Rig, TEXT("/auth/refresh"))).Request;
		TestTrue(TEXT("idle: no timeout of its own -- the engine's defaults"), Sent.Timeout == 0.f && Sent.ActivityTimeout == 0.f);
		Rig.Clock->Advance(29.0);
		Rig.Drain();
		TestFalse(TEXT("idle: still waiting at 29 s"), Parked->IsDone());
		Rig.Clock->Advance(1.0);
		if (!TestTrue(TEXT("idle: completes at the 30 s activity timeout"), Rig.Wait(Parked)))
		{
			Abandon(Rig);
			return false;
		}
		TestTrue(TEXT("idle: Connection, retryable"), Parked->Error.Kind == EVaRestErrorKind::Connection && Parked->Error.bRetryable);
		TestTrue(TEXT("idle: the session is kept"), Rig.Client.GetSession().HasRefreshToken() && Rig.Client.GetSession().AccessToken == TEXT("old-access-123456"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshResponseRedactedTest, "VaRest.Http.Auth.RefreshResponseRedacted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshResponseRedactedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// The refresh response holds the new tokens before FinishRefresh makes them session secrets (layer 3), and the log
	// stage and the trace read it first: the log stage takes its redactor as the outcome comes back (VaRestLogStage.cpp:130),
	// before the refresh ends. Layer 2 -- each token path's last segment as a JSON key -- is what masks them there. Paths
	// off the defaults: "bearer" and "renewal" match no built-in rule (VaRestJsonRedact.h:20-24), so none hides a missing key.
	TGuardConsoleVariable<int32> Level(TEXT("VaRest.LogLevel"), -1);
	FVaRestBearerRefreshConfig Config;
	Config.AccessTokenPath = TEXT("data.bearer");
	Config.RefreshTokenPath = TEXT("/auth/renewal");
	FTestRig Rig;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Body).WithAuth(FVaRestAuth::BearerRefresh(Config));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	RefreshServer(Rig, Json(200, R"({"data":{"bearer":"new-access-123456"},"auth":{"renewal":"new-refresh-123456"},"expiresIn":60})"));
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Headers | EVaRestTraceDetail::Bodies, 4096);
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("-> 401"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
	if (!TestTrue(TEXT("the 401, the refresh, the replay"), Rig.Wait(Outcome) && Rig.Drain() && Rig.Fake->NumCalls() == 3 && Outcome->SuccessCalls.load() == 1))
	{
		Abandon(Rig);
		return false;
	}
	FlushRedactionPipe();
	const FVaRestSessionData Data = Rig.Client.GetSession();
	TestTrue(TEXT("the tokens were read at the configured paths"), Data.AccessToken == TEXT("new-access-123456") && Data.RefreshToken == TEXT("new-refresh-123456"));
	const uint64 RefreshId = Rig.Fake->GetCall(1).Request.RequestId;
	const FString Prefix = FString::Printf(TEXT("#%llu < body"), RefreshId);
	bool bResponseLine = false;
	for (const FString& Line : Capture.GetLines())
	{
		for (const TCHAR* Token : RefreshTokens)
		{
			if (Line.Contains(Token))
			{
				AddError(FString::Printf(TEXT("a log line holds %s: %s"), Token, *Line));
			}
		}
		bResponseLine |= Line.Contains(Prefix) && Line.Contains(TEXT("\"bearer\":\"***\"")) && Line.Contains(TEXT("\"renewal\":\"***\""));
	}
	TestTrue(TEXT("the refresh response's body line was written, both tokens masked by their keys"), bResponseLine);
	TArray<FString> Traced = TraceStrings(*Scoped.Recorder, RefreshId);
	TestTrue(TEXT("the trace saw the refresh request"), Traced.Num() > 0);
	for (const FVaRestTraceExcerptEvent& Excerpt : Scoped.Recorder->ExcerptsOf(RefreshId))
	{
		Traced.Add(Excerpt.Excerpt.ToText());
	}
	for (const FString& Value : Traced)
	{
		for (const TCHAR* Token : RefreshTokens)
		{
			if (Value.Contains(Token))
			{
				AddError(FString::Printf(TEXT("a trace field of the refresh request holds %s: %s"), Token, *Value));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthPathKeySyntaxTest, "VaRest.Http.Auth.TokenPathKeysFollowTheSyntax",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthPathKeySyntaxTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// A token path's key is read in the path's own syntax (VaRestJsonPath.cpp:158-160): a pointer ends at its last '/' and
	// unescapes RFC 6901's ~1 and ~0; a dotted path ends at its last '.', and a '/' in it is part of a name; trailing array
	// indexes go in both. No key below matches a built-in rule (VaRestJsonRedact.h:20-24).
	struct FCase
	{
		const TCHAR* Path;
		const TCHAR* Key;
		/** What splitting on both '.' and '/' would add instead: a key the response does not hold */
		const TCHAR* NotKey;
	};
	const FCase Cases[] = {
		{ TEXT("/grant/grant.value"), TEXT("grant.value"), TEXT("value") },
		{ TEXT("/grant/access~1bearer"), TEXT("access/bearer"), TEXT("access~1bearer") },
		{ TEXT("data.renew/al[0]"), TEXT("renew/al"), TEXT("al") },
		// A trailing array index, as "[0]" above: the array's key, not one "0" every body has
		{ TEXT("/grant/list/0"), TEXT("list"), TEXT("0") },
	};
	for (const FCase& Case : Cases)
	{
		FVaRestBearerRefreshConfig Config;
		Config.AccessTokenPath = Case.Path;
		FTestRig Rig;
		Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(Config));
		const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Redactor = VaRest::Http::FClientAccess::State(Rig.Client)->GetRedactor();
		TestTrue(FString::Printf(TEXT("%s: the key %s"), Case.Path, Case.Key), Redactor->GetRules().ShouldRedact(FStringView(Case.Key), EVaRestRedactKeyKind::Json));
		TestFalse(FString::Printf(TEXT("%s: not %s"), Case.Path, Case.NotKey), Redactor->GetRules().ShouldRedact(FStringView(Case.NotKey), EVaRestRedactKeyKind::Json));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshIdUniqueTest, "VaRest.Http.Auth.BearerRefreshIdIsProcessUnique",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshIdUniqueTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Spec review I-2: a refresh's id is a process-unique request id (FVaRestTrace::NewRequestId, VaRestSession.cpp:486), so
	// two clients' refresh requests are two Inspector records. VaRest.Http.Auth.BearerRefresh cannot see a per-client
	// number: the refresh request carries whatever id its refresh has (VaRestClient.cpp:171), so the two always agree.
	uint64 Ids[2] = {};
	for (uint64& Id : Ids)
	{
		FTestRig Rig;
		Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
		RefreshServer(Rig, NewTokens());
		auto Outcome = MakeOutcome<FVaRestJson>();
		const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
		if (!TestTrue(TEXT("the 401, the refresh, the replay"), Rig.Wait(Outcome) && Rig.Fake->NumCalls() == 3))
		{
			Abandon(Rig);
			return false;
		}
		Id = Rig.Fake->GetCall(1).Request.RequestId;
		TestNotEqual(TEXT("not the id of the request it was for"), Id, Handle.GetId());
	}
	TestNotEqual(TEXT("two clients' refresh requests, two ids"), Ids[0], Ids[1]);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshCapTest, "VaRest.Http.Auth.RefreshResponseOverTheCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshCapTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// The threading rule: the refresh response is parsed on a task, bounded by the client's MaxJsonBytes (the transport
	// stops the body at it, VaRestPipeline.cpp:544), so a hostile endpoint's huge answer costs no more than the cap. A
	// success that cannot be read is the endpoint's own "no" (ruling I-7): Rejected, as one that is not JSON.
	FTestRig Rig;
	Rig.Client.WithMaxJsonBytes(64).WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	// 85 bytes
	RefreshServer(Rig, NewTokens());
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome) && Rig.Drain() && Rig.Fake->NumCalls() >= 2))
	{
		Abandon(Rig);
		return false;
	}
	TestEqual(TEXT("the 401 and the refresh, no replay"), Rig.Fake->NumCalls(), 2);
	TestTrue(TEXT("the refresh request's body is capped at MaxJsonBytes"), Rig.Fake->GetCall(1).Request.MaxBodyBytes == 64);
	TestTrue(TEXT("the parked request: Kind Auth, not retryable"), Outcome->Error.Kind == EVaRestErrorKind::Auth && !Outcome->Error.bRetryable);
	TestFalse(TEXT("the session expired"), Rig.Client.GetSession().HasRefreshToken());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthBearerConfigErrorsTest, "VaRest.Http.Auth.BearerRefreshConfigErrors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthBearerConfigErrorsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 18 review I-1, ruling Q7: a configuration that cannot work fails each request Kind=Config with nothing sent, and
	// keeps the session -- every refresh would be Rejected, a logout for a typo
	struct FCase
	{
		const TCHAR* Name;
		const TCHAR* Logged;
		FVaRestBearerRefreshConfig Config;
	};
	TArray<FCase> Cases;
	{
		FCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = TEXT("an empty AccessTokenPath");
		Case.Logged = TEXT("FVaRestAuth::BearerRefresh: AccessTokenPath is empty");
		Case.Config.AccessTokenPath.Reset();
	}
	{
		FCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = TEXT("an empty RefreshPath");
		Case.Logged = TEXT("FVaRestAuth::BearerRefresh: RefreshPath is empty");
		Case.Config.RefreshPath = TEXT("  ");
	}
	{
		FCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = TEXT("the Json style with an empty RefreshBodyField");
		Case.Logged = TEXT("FVaRestAuth::BearerRefresh: the Json style's RefreshBodyField is empty");
		Case.Config.RefreshBodyField.Reset();
	}
	for (const TCHAR* Path : { TEXT("data..token"), TEXT("data[0"), TEXT("data[01]"), TEXT("/data/~2") })
	{
		FCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = Path;
		Case.Logged = TEXT("FVaRestAuth::BearerRefresh: AccessTokenPath has invalid syntax");
		Case.Config.AccessTokenPath = Path;
	}
	{
		FCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = TEXT("an invalid optional refresh token path");
		Case.Logged = TEXT("FVaRestAuth::BearerRefresh: RefreshTokenPath has invalid syntax");
		Case.Config.RefreshTokenPath = TEXT("data..refresh");
	}
	{
		FCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = TEXT("an invalid optional expiry path");
		Case.Logged = TEXT("FVaRestAuth::BearerRefresh: ExpiresInPath has invalid syntax");
		Case.Config.ExpiresInPath = TEXT("/data/~2");
	}
	for (const FCase& Case : Cases)
	{
		AddExpectedErrorPlain(Case.Logged, EAutomationExpectedErrorFlags::Contains, 1);
		FTestRig Rig;
		Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(Case.Config));
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
		RefreshServer(Rig, NewTokens());
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
		if (!TestTrue(FString::Printf(TEXT("%s: completes"), Case.Name), Rig.Wait(Outcome) && Rig.Drain()))
		{
			Abandon(Rig);
			return false;
		}
		TestTrue(FString::Printf(TEXT("%s: Kind Config, not retryable"), Case.Name), Outcome->Error.Kind == EVaRestErrorKind::Config && !Outcome->Error.bRetryable);
		TestEqual(FString::Printf(TEXT("%s: nothing sent"), Case.Name), Rig.Fake->NumCalls(), 0);
		const FVaRestSessionData Data = Rig.Client.GetSession();
		TestTrue(FString::Printf(TEXT("%s: the session kept"), Case.Name), Data.AccessToken == TEXT("old-access-123456") && Data.RefreshToken == TEXT("old-refresh-123456"));
	}
	FVaRestBearerRefreshConfig OptionalPaths;
	OptionalPaths.RefreshTokenPath.Reset();
	OptionalPaths.ExpiresInPath.Reset();
	TestTrue(TEXT("optional empty response paths remain valid"), FVaRestAuth::BearerRefresh(OptionalPaths)->GetConfigError().IsEmpty());
	// Not Q7: the OAuth2 form never reads RefreshBodyField, and an empty RefreshStatuses refreshes before a known expiry only
	{
		FVaRestBearerRefreshConfig Config;
		Config.Style = EVaRestRefreshStyle::OAuth2Form;
		Config.RefreshBodyField.Reset();
		FTestRig Rig;
		Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(Config));
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
		RefreshServer(Rig, NewTokens());
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
		TestTrue(TEXT("OAuth2Form, no RefreshBodyField: the 401, the refresh, the replay succeeds"), Rig.Wait(Outcome) && Rig.Drain()
			&& Outcome->SuccessCalls.load() == 1 && Rig.Fake->NumCalls() == 3);
	}
	{
		FVaRestBearerRefreshConfig Config;
		Config.RefreshStatuses.Reset();
		FTestRig Rig;
		Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(Config));
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 20.0);
		RefreshServer(Rig, NewTokens());
		auto Refused = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/p/1")), Refused).Send();
		TestTrue(TEXT("no RefreshStatuses: a 401 is the answer, no refresh"), Rig.Wait(Refused) && Rig.Drain() && Refused->Error.Kind == EVaRestErrorKind::Http
			&& Refused->Error.HttpStatus == 401 && Rig.Fake->NumCalls() == 1);
		Rig.Clock->Advance(10.0);
		auto Due = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/p/2")), Due).Send();
		TestTrue(TEXT("... and a due expiry still refreshes first"), Rig.Wait(Due) && Rig.Drain() && Due->SuccessCalls.load() == 1 && Due->Response.Attempts == 1
			&& Rig.Fake->NumCalls() == 3 && FindCall(Rig, TEXT("/auth/refresh")) == 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthBearerConfigNoRefreshTest, "VaRest.Http.Auth.BearerRefreshConfigErrorSendsNoRefresh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthBearerConfigNoRefreshTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 18 review I-1: with an unusable configuration a session that is due -- expired, or with a refresh token only --
	// sends no refresh, which would be Rejected and expire it. Every refresh door asks CanRefreshNow (M4a final review B5,
	// VaRestAuth.cpp:256-260), and BearerRefresh's own CanRefresh is false then too (VaRestBearerRefresh.cpp:192-199)
	AddExpectedErrorPlain(TEXT("FVaRestAuth::BearerRefresh: AccessTokenPath is empty"), EAutomationExpectedErrorFlags::Contains, 3);
	FVaRestBearerRefreshConfig Config;
	Config.AccessTokenPath.Reset();
	struct FDue
	{
		const TCHAR* Name;
		const TCHAR* Access;
		double ExpiresIn;
	};
	const FDue Sessions[] = {
		{ TEXT("expired"), TEXT("old-access-123456"), 20.0 },
		{ TEXT("a refresh token only"), TEXT(""), 0.0 },
	};
	for (const FDue& Session : Sessions)
	{
		FTestRig Rig;
		Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(Config));
		Rig.Client.SetSession(Session.Access, TEXT("old-refresh-123456"), Session.ExpiresIn);
		RefreshServer(Rig, NewTokens());
		Rig.Clock->Advance(25.0);
		TestFalse(FString::Printf(TEXT("%s: not due -- it cannot refresh"), Session.Name), Rig.Client.NeedsAuthRefresh());
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
		if (!TestTrue(FString::Printf(TEXT("%s: completes"), Session.Name), Rig.Wait(Outcome) && Rig.Drain()))
		{
			Abandon(Rig);
			return false;
		}
		TestTrue(FString::Printf(TEXT("%s: Kind Config"), Session.Name), Outcome->Error.Kind == EVaRestErrorKind::Config);
		TestEqual(FString::Printf(TEXT("%s: no refresh call, nothing sent"), Session.Name), Rig.Fake->NumCalls(), 0);
		TestTrue(FString::Printf(TEXT("%s: the session kept"), Session.Name), Rig.Client.GetSession().RefreshToken == TEXT("old-refresh-123456"));
	}
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(Config));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	RefreshServer(Rig, NewTokens());
	const FRefreshAnswers Answers = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Answers));
	TestTrue(TEXT("RefreshAuth: (Failed, Config)"), PumpUntil([Answers] { return Answers->Num() == 1; }) && Rig.Drain()
		&& (*Answers)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Failed, EVaRestErrorKind::Config));
	TestEqual(TEXT("RefreshAuth: nothing sent"), Rig.Fake->NumCalls(), 0);
	TestTrue(TEXT("RefreshAuth: the session kept"), Rig.Client.GetSession().RefreshToken == TEXT("old-refresh-123456"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthConfigErrorNoRefreshTest, "VaRest.Http.Auth.ConfigErrorRefreshesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthConfigErrorNoRefreshTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// M4a final review B5 (review B m-3): one rule, CanRefreshNow, decides whether a refresh may start. A custom auth whose
	// GetConfigError is set while its CanRefresh and NeedsRefresh say yes sends no refresh -- not proactively from the gate,
	// not through RefreshAuth -- and NeedsAuthRefresh says none is due. Its requests fail Kind=Config at once, also while
	// another auth's refresh is in flight, which they do not wait out (the gate passes them straight on to AuthApply).
	FTestRig Rig;
	const TSharedRef<FMisconfiguredRefresher, ESPMode::ThreadSafe> Broken = MakeShared<FMisconfiguredRefresher, ESPMode::ThreadSafe>();
	Rig.Client.WithAuth(Broken);
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	auto Proactive = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/p")), Proactive).Send();
	if (!TestTrue(TEXT("a request of the misconfigured auth completes"), Rig.Wait(Proactive) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... Kind Config (AuthInvalid), nothing sent"), Proactive->Error.Kind == EVaRestErrorKind::Config
		&& Proactive->Error.Message.Contains(TEXT("AuthInvalid")) && Rig.Fake->NumCalls() == 0);
	TestEqual(TEXT("... and no proactive refresh"), Broken->Refreshes, 0);
	TestFalse(TEXT("NeedsAuthRefresh: no refresh is due"), Rig.Client.NeedsAuthRefresh());
	const FRefreshAnswers Answers = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Answers));
	TestTrue(TEXT("RefreshAuth: (Failed, Config)"), PumpUntil([Answers] { return Answers->Num() == 1; })
		&& (*Answers)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Failed, EVaRestErrorKind::Config));
	TestEqual(TEXT("... and no refresh"), Broken->Refreshes, 0);
	Rig.Drain();

	// Another auth's refresh is in flight (WithAuth does not end it, Task 16 review M-5): the misconfigured auth's request
	// is refused before it ends
	FTestRig Busy;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Hand = HandRig(Busy);
	const FRefreshAnswers Running = MakeAnswers();
	Busy.Client.RefreshAuth(Record(Running));
	const TSharedRef<FMisconfiguredRefresher, ESPMode::ThreadSafe> Second = MakeShared<FMisconfiguredRefresher, ESPMode::ThreadSafe>();
	Busy.Client.WithAuth(Second);
	auto Refused = MakeOutcome<FVaRestJson>();
	Observe(Busy.Client.Get(TEXT("/q")), Refused).Send();
	const bool bRefused = Busy.Wait(Refused, 1.0);
	TestTrue(TEXT("with another auth's refresh in flight: refused before that refresh ends, Kind Config"), bRefused && Hand->IsPending()
		&& Refused->Error.Kind == EVaRestErrorKind::Config);
	TestEqual(TEXT("... and the misconfigured auth refreshed nothing"), Second->Refreshes, 0);
	Hand->Answer(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Connection, TEXT("no route to the refresh endpoint")));
	TestTrue(TEXT("the other refresh ends"), PumpUntil([Running] { return Running->Num() == 1; }) && Busy.Wait(Refused) && Busy.Drain());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshOtherHostTest, "VaRest.Http.Auth.RefreshAnsweredByAnotherHost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshOtherHostTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 18 review M-2 (ruled): a 2xx that is not JSON, ending at another host than the refresh URL's -- a captive portal's
	// login page after a redirect -- is Failed(Connection), the session kept. The control: the same answer from the refresh
	// URL's own host, written in another case (RFC 3986 3.2.2), is the endpoint's own and Rejected.
	struct FCase
	{
		const TCHAR* Name;
		const TCHAR* EffectiveUrl;
		EVaRestErrorKind Kind;
		bool bRetryable;
		bool bExpires;
	};
	const FCase Cases[] = {
		{ TEXT("another host"), TEXT("https://portal.example/login?from=api"), EVaRestErrorKind::Connection, true, false },
		{ TEXT("the refresh URL's host"), TEXT("https://API.Test/login"), EVaRestErrorKind::Auth, false, true },
	};
	for (const FCase& Case : Cases)
	{
		FVaRestFakeResponse Portal = FVaRestFakeResponse::Text(200, TEXT("<html>sign in to the hotel wifi</html>"), TEXT("text/html"));
		Portal.EffectiveUrl = Case.EffectiveUrl;
		FTestRig Rig;
		Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
		RefreshServer(Rig, Portal);
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
		if (!TestTrue(FString::Printf(TEXT("%s: completes"), Case.Name), Rig.Wait(Outcome) && Rig.Drain() && Rig.Fake->NumCalls() == 2))
		{
			Abandon(Rig);
			return false;
		}
		TestEqual(FString::Printf(TEXT("%s: the parked request's kind"), Case.Name), Outcome->Error.Kind, Case.Kind);
		TestEqual(FString::Printf(TEXT("%s: ... retryable or not"), Case.Name), Outcome->Error.bRetryable, Case.bRetryable);
		TestEqual(FString::Printf(TEXT("%s: the session expired or kept"), Case.Name), !Rig.Client.GetSession().HasRefreshToken(), Case.bExpires);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshOutcomesMoreTest, "VaRest.Http.Auth.RefreshOutcomesRejected",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshOutcomesMoreTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 18 review M-6: more of ruling I-7's "no" -- the common refusals (a 404 is a wrong RefreshPath: a logout, by the
	// ruling), and a success with nothing in it (a null document, VaRestDecode.cpp:79-83: no access token)
	struct FCase
	{
		const TCHAR* Name;
		FVaRestFakeResponse Answer;
	};
	const FCase Cases[] = {
		{ TEXT("401"), Json(401, R"({"error":"invalid_token"})") },
		{ TEXT("403"), Json(403) },
		{ TEXT("404"), Json(404) },
		{ TEXT("an empty 200"), Json(200, "") },
		{ TEXT("204"), Json(204, "") },
	};
	for (const FCase& Case : Cases)
	{
		FTestRig Rig;
		Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
		RefreshServer(Rig, Case.Answer);
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
		if (!TestTrue(FString::Printf(TEXT("%s: completes"), Case.Name), Rig.Wait(Outcome) && Rig.Drain()))
		{
			Abandon(Rig);
			return false;
		}
		TestTrue(FString::Printf(TEXT("%s: rejected -- the parked request Kind Auth, not retryable"), Case.Name),
			Outcome->Error.Kind == EVaRestErrorKind::Auth && !Outcome->Error.bRetryable);
		TestFalse(FString::Printf(TEXT("%s: the session expired"), Case.Name), Rig.Client.GetSession().HasRefreshToken());
		TestEqual(FString::Printf(TEXT("%s: the 401 and the refresh, no replay"), Case.Name), Rig.Fake->NumCalls(), 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshUnknownExpiryTest, "VaRest.Http.Auth.RefreshUnknownExpiry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshUnknownExpiryTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 18 review M-6: an expiresIn that is missing, negative or not a number is unknown (0): no expiry to refresh
	// before, so the next request goes without a refresh -- no loop
	struct FCase
	{
		const TCHAR* Name;
		const char* Body;
	};
	const FCase Cases[] = {
		{ TEXT("missing"), R"({"accessToken":"new-access-123456"})" },
		{ TEXT("negative"), R"({"accessToken":"new-access-123456","expiresIn":-5})" },
		{ TEXT("not a number"), R"({"accessToken":"new-access-123456","expiresIn":"soon"})" },
	};
	for (const FCase& Case : Cases)
	{
		FTestRig Rig;
		Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
		RefreshServer(Rig, Json(200, Case.Body));
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/p/1")), Outcome).Send();
		if (!TestTrue(FString::Printf(TEXT("%s: the 401, the refresh, the replay"), Case.Name), Rig.Wait(Outcome) && Rig.Drain() && Outcome->SuccessCalls.load() == 1))
		{
			Abandon(Rig);
			return false;
		}
		const FVaRestSessionData Data = Rig.Client.GetSession();
		TestTrue(FString::Printf(TEXT("%s: refreshed, the expiry and lifetime unknown"), Case.Name), Data.AccessToken == TEXT("new-access-123456")
			&& Data.ExpiresAt == 0.0 && Data.Lifetime == 0.0);
		TestFalse(FString::Printf(TEXT("%s: not due"), Case.Name), Rig.Client.NeedsAuthRefresh());
		auto Next = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/p/2")), Next).Send();
		TestTrue(FString::Printf(TEXT("%s: the next request goes at once, with no refresh"), Case.Name), Rig.Wait(Next) && Rig.Drain() && Next->SuccessCalls.load() == 1
			&& Rig.Fake->NumCalls() == 4 && FindCall(Rig, TEXT("/auth/refresh"), 1) == INDEX_NONE);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshRequestShapeTest, "VaRest.Http.Auth.RefreshRequestShape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshRequestShapeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 18 review M-3: Accept: application/json in both styles, so an endpoint that negotiates the format answers JSON.
	// Review M-6: a form refresh token's '+', '/' and '=' arrive percent-encoded (FormEncode, VaRestUrl.h:14-15), so the
	// server does not read '+' as a space or '=' as a new field.
	for (const EVaRestRefreshStyle Style : { EVaRestRefreshStyle::Json, EVaRestRefreshStyle::OAuth2Form })
	{
		const bool bForm = Style == EVaRestRefreshStyle::OAuth2Form;
		const TCHAR* StyleName = bForm ? TEXT("OAuth2Form") : TEXT("Json");
		FVaRestBearerRefreshConfig Config;
		Config.Style = Style;
		FTestRig Rig;
		Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(Config));
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old+refresh/123="));
		RefreshServer(Rig, NewTokens());
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
		if (!TestTrue(FString::Printf(TEXT("%s: the 401, the refresh, the replay"), StyleName), Rig.Wait(Outcome) && Rig.Drain() && Rig.Fake->NumCalls() == 3))
		{
			Abandon(Rig);
			return false;
		}
		const FVaRestTransportRequest& Refresh = Rig.Fake->GetCall(1).Request;
		TestEqualSensitive(FString::Printf(TEXT("%s: Accept"), StyleName), Refresh.Headers.Get(TEXT("Accept")), FString(TEXT("application/json")));
		TestEqualSensitive(FString::Printf(TEXT("%s: the body"), StyleName), BodyText(Rig.Fake->ReadBody(1)), FString(bForm
			? TEXT("grant_type=refresh_token&refresh_token=old%2Brefresh%2F123%3D")
			: TEXT(R"({"refreshToken":"old+refresh/123="})")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthReplayBodiesTest, "VaRest.Http.Auth.ReplayBodies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthReplayBodiesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Carry (e), plan decision 24: a session replay is a new transport request carrying the whole body again, a stream body
	// re-created -- the engine's Apple request takes no new content while in flight and makes its NSInputStream once per
	// setup (AppleHttp.cpp:627-634, :751-753), so a stream cannot be resent in place. Resolve copies the body when an auth
	// that can refresh may replay it (VaRestPipeline.cpp:814-816), the transport stage then hands each attempt its own copy
	// (VaRestPipeline.cpp:523-538), and the gate replays only a body it can send again (VaRestAuthGate.cpp:303). BearerRefresh,
	// so the 401s, the refresh request and the replays all go through the real path; multipart (with a file part), BodyFile
	// and JSON: the three body shapes.
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaRestAuthTest") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Dir, true);
	// Declared before the rigs, so it runs after they are gone (and with them every request that reads the files)
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Dir, false, true); };
	TArray<uint8> Pattern;
	for (int32 Index = 0; Index < 1000; ++Index)
	{
		Pattern.Add(static_cast<uint8>(Index * 11 + 5));
	}
	const FString PartFile = Dir / TEXT("part.bin");
	const FString UploadFile = Dir / TEXT("upload.bin");
	if (!TestTrue(TEXT("fixtures written"), FFileHelper::SaveArrayToFile(Pattern, *PartFile) && FFileHelper::SaveArrayToFile(Pattern, *UploadFile)))
	{
		return false;
	}
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	RefreshServer(Rig, NewTokens());
	auto Parts = MakeOutcome<FVaRestJson>();
	auto File = MakeOutcome<FVaRestJson>();
	auto Posted = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Post(TEXT("/parts")).Body(FVaRestMultipart().AddField(TEXT("f"), TEXT("v")).AddFile(TEXT("file"), PartFile)), Parts).Send();
	Observe(Rig.Client.Put(TEXT("/file")).BodyFile(UploadFile), File).Send();
	Observe(Rig.Client.Post(TEXT("/json")).Body(FVaRestJson::Object().Set(TEXT("a"), 1)), Posted).Send();
	if (!TestTrue(TEXT("all complete"), PumpUntil([Parts, File, Posted] { return Parts->IsDone() && File->IsDone() && Posted->IsDone(); }) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("all three succeed on their replay"), Parts->SuccessCalls.load() == 1 && File->SuccessCalls.load() == 1 && Posted->SuccessCalls.load() == 1);
	// Each 401 that comes back after the refresh ended replays at once (newer credentials), so one refresh however they land
	TestTrue(TEXT("one refresh for the three 401s"), FindCall(Rig, TEXT("/auth/refresh")) != INDEX_NONE && FindCall(Rig, TEXT("/auth/refresh"), 1) == INDEX_NONE);
	for (const TCHAR* Path : { TEXT("/parts"), TEXT("/file"), TEXT("/json") })
	{
		const int32 First = FindCall(Rig, Path, 0);
		const int32 Second = FindCall(Rig, Path, 1);
		if (!TestTrue(FString::Printf(TEXT("%s: two attempts"), Path), First != INDEX_NONE && Second != INDEX_NONE))
		{
			continue;
		}
		const TArray<uint8> Sent = Rig.Fake->ReadBody(First);
		TestTrue(FString::Printf(TEXT("%s: the replay sends the whole body again"), Path), Sent.Num() > 0 && Sent == Rig.Fake->ReadBody(Second));
		TestEqualSensitive(FString::Printf(TEXT("%s: ... with the new token"), Path), Rig.Fake->GetCall(Second).Request.Headers.Get(TEXT("Authorization")),
			FString(TEXT("Bearer new-access-123456")));
	}

	// Plan decision 8: an auth that cannot refresh never replays, so its POST body is moved, not copied
	FTestRig Plain;
	Plain.Client.WithBearer(TEXT("old-access-123456"));
	RefreshServer(Plain, NewTokens());
	auto Once = MakeOutcome<FVaRestJson>();
	Observe(Plain.Client.Post(TEXT("/json")).Body(FVaRestJson::Object().Set(TEXT("a"), 1)), Once).Send();
	if (!TestTrue(TEXT("plain Bearer: completes"), Plain.Wait(Once) && Plain.Drain()))
	{
		Abandon(Plain);
		return false;
	}
	TestTrue(TEXT("plain Bearer: the 401 is the answer, one call"), Once->Error.HttpStatus == 401 && Plain.Fake->NumCalls() == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRetryAroundTest, "VaRest.Http.Auth.RetryAroundTheGate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRetryAroundTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Plan decision 2: Retry is outside AuthGate (VaRestClientState.cpp:225-228). A refresh that fails on the network (a 503:
	// Failed(Connection), ruling I-7) fails the parked request as a Connection error, which Retry retries; the next attempt
	// meets the old token again and starts a new refresh, which works.
	FTestRig Rig;
	FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(2);
	Policy.Backoff.Jitter = EVaRestJitter::None;
	Rig.Client.WithRetry(Policy).WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	TSharedRef<int32, ESPMode::ThreadSafe> Refreshes = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Rig.Fake->SetResponder([Refreshes](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.Contains(TEXT("/auth/")))
		{
			return ++*Refreshes == 1 ? Json(503) : NewTokens();
		}
		return Request.Headers.Get(TEXT("Authorization")) == TEXT("Bearer new-access-123456") ? Json(200) : Json(401);
	});
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
	// The backoff is spec 4.1's first step: BaseDelay 0.5 s, no jitter
	if (!TestTrue(TEXT("the refresh failed; Retry backs off"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2 && Rig.Clock->NumPendingTimers() == 1; }) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	TestFalse(TEXT("... the request not answered yet"), Outcome->IsDone());
	TestTrue(TEXT("the session survived the failed refresh"), Rig.Client.GetSession().HasRefreshToken()
		&& Rig.Client.GetSession().AccessToken.Equals(TEXT("old-access-123456"), ESearchCase::CaseSensitive));
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("a success: attempt 1 (401), the retry (401), its replay (200)"), Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 3);
	TestEqual(TEXT("two refreshes"), *Refreshes, 2);
	TestEqual(TEXT("calls: /p, refresh, /p, refresh, /p"), Rig.Fake->NumCalls(), 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthBypassTest, "VaRest.Http.Auth.RefreshBypassesLimiter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthBypassTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Ruling M-8: the refresh request skips the limiter's queue (VaRestPipeline.cpp:368-377). One slot, which a long poll
	// (NoAuth, never answered) takes when A's 401 frees it; A's refresh still reaches the transport, and A's replay then waits
	// its turn. Beside VaRest.Http.Auth.BearerRefreshPassesTheGate (the limiter's books, two parks): the count at the
	// transport that WithMaxConcurrent's doc promises, with a NoAuth() request holding the one slot.
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1).WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
	if (!TestTrue(TEXT("a client state"), State.IsValid()))
	{
		return false;
	}
	VaRest::Http::FLimiter& Limiter = State->GetLimiter();
	auto A = MakeOutcome<FVaRestJson>();
	auto Poll = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), A).Send();
	if (!TestTrue(TEXT("A holds the slot"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Observe(Rig.Client.Get(TEXT("/poll")).NoAuth(), Poll).Send();
	Rig.Fake->Complete(0, Json(401));
	if (!TestTrue(TEXT("the poll takes the slot and the refresh goes out anyway"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3; }) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	const int32 RefreshCall = FindCall(Rig, TEXT("/auth/refresh"));
	const int32 PollCall = FindCall(Rig, TEXT("/poll"));
	if (!TestTrue(TEXT("both at the transport"), RefreshCall != INDEX_NONE && PollCall != INDEX_NONE))
	{
		Abandon(Rig);
		return false;
	}
	TestEqual(TEXT("... MaxConcurrent + 1 out during the refresh (WithMaxConcurrent's doc)"), Rig.Fake->NumInFlight(), 2);
	TestTrue(TEXT("... the poll holds the one slot"), Limiter.NumActive() == 1 && Limiter.NumQueued() == 0);
	Rig.Fake->Complete(RefreshCall, NewTokens());
	if (!TestTrue(TEXT("refreshed"), PumpUntil([&Rig] { return Rig.Client.GetSession().AccessToken == TEXT("new-access-123456"); }) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("A's replay waits its turn in the queue: every attempt passes the limiter (spec 4.6)"), Rig.Fake->NumCalls() == 3 && Limiter.NumQueued() == 1);
	Rig.Fake->Complete(PollCall, Json(200));
	if (!TestTrue(TEXT("then A replays"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 4; })))
	{
		Abandon(Rig);
		return false;
	}
	TestEqualSensitive(TEXT("... with the new token"), Rig.Fake->GetCall(3).Request.Headers.Get(TEXT("Authorization")), FString(TEXT("Bearer new-access-123456")));
	Rig.Fake->Complete(3, Json(200));
	if (!TestTrue(TEXT("both complete"), PumpUntil([A, Poll] { return A->IsDone() && Poll->IsDone(); }) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("A succeeds on its replay"), A->SuccessCalls.load() == 1 && A->Response.Attempts == 2);
	TestTrue(TEXT("the poll finished"), Poll->SuccessCalls.load() == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthDeadlineParkTest, "VaRest.Http.Auth.DeadlineWhileParked",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthDeadlineParkTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Concurrency review M-1: a park is bounded by the request's retry deadline (VaRestAuthGate.cpp:202-205) -- an auth may
	// hold its refresh open (a re-login screen, ruling I-4) -- and ends as Timeout (Deadline), not retryable, out of the
	// refresh, which goes on. Beside VaRest.Http.Auth.ParkEndsAtItsDeadline (a replay's park, retry disabled): a park before
	// the first attempt, under a policy that would retry, which still does not retry a Deadline.
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	// Due at once for FHandAuth (expiry within its 30 s): the request parks before its first attempt
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 10.0);
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
	if (!TestTrue(TEXT("a client state"), State.IsValid()))
	{
		return false;
	}
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/p")).Retry(FVaRestRetryPolicy::Exponential(1).WithDeadline(2.f)), Outcome).Send();
	Rig.Drain();
	if (!TestTrue(TEXT("parked before its first attempt"), Rig.Fake->NumCalls() == 0 && Auth->IsPending() && State->NumRefreshWaitersInternal() == 1))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(1.999);
	Rig.Drain();
	TestFalse(TEXT("not before the deadline"), Outcome->IsDone());
	Rig.Clock->Advance(0.001);
	if (!TestTrue(TEXT("ends at the deadline"), Rig.Wait(Outcome) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... Timeout, not retryable, saying Deadline"), Outcome->Error.Kind == EVaRestErrorKind::Timeout && !Outcome->Error.bRetryable
		&& Outcome->Error.Message.Contains(TEXT("Deadline")));
	TestTrue(TEXT("... once, and nothing left armed: neither the park's timer nor a backoff"), Outcome->CompleteCalls.load() == 1 && Rig.Clock->NumPendingTimers() == 0);
	TestEqual(TEXT("... out of the refresh"), State->NumRefreshWaitersInternal(), 0);
	TestTrue(TEXT("... nothing sent, the refresh still running"), Rig.Fake->NumCalls() == 0 && State->IsRefreshing() && Auth->IsPending());
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	Rig.Drain();
	TestTrue(TEXT("the refresh's end sends nothing for it"), Rig.Fake->NumCalls() == 0 && Outcome->CompleteCalls.load() == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthHeaderChangeTest, "VaRest.Http.Auth.HeaderChangeKeepsTheReplay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthHeaderChangeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Concurrency review M-6: a session header set while a request is out does not make its 401 a "newer credentials"
	// replay -- that would spend its one replay on the same expired token; it refreshes instead. SetSessionHeader moves
	// Generation, not CredentialGeneration (VaRestSession.cpp:263), which is what the gate compares (VaRestAuthGate.cpp:305).
	// The positive half (a token change replays at once) is VaRest.Http.Auth.NewerCredentialsReplayAtOnce.
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> OldTokenCalls = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	Rig.Fake->SetResponder([OldTokenCalls](const FVaRestTransportRequest& Request)
	{
		if (Request.Headers.Get(TEXT("Authorization")) == TEXT("Bearer new-access-123456"))
		{
			return Json(200);
		}
		// The first is held for the test to answer; any later one with the old token is refused at once
		return ++*OldTokenCalls == 1 ? FVaRestFakeResponse::Manual() : Json(401);
	});
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), Outcome).Send();
	if (!TestTrue(TEXT("out with the old token"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Client.SetSessionHeader(TEXT("X-Session-ID"), TEXT("sid-1234567890"));
	Rig.Fake->Complete(0, Json(401));
	if (!TestTrue(TEXT("the 401 parks behind a refresh"), PumpUntil([&Auth] { return Auth->IsPending(); }) && Rig.Drain() && Rig.Fake->NumCalls() == 1))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... and its replay succeeds"), Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 2);
	TestEqual(TEXT("one refresh"), Auth->Refreshes, 1);
	if (!TestEqual(TEXT("two calls: the 401 and the replay"), Rig.Fake->NumCalls(), 2))
	{
		return false;
	}
	const FVaRestHeaders& Replayed = Rig.Fake->GetCall(1).Request.Headers;
	TestTrue(TEXT("the replay carries the new token and the header set meanwhile"), Replayed.Get(TEXT("Authorization")) == TEXT("Bearer new-access-123456")
		&& Replayed.Get(TEXT("X-Session-ID")) == TEXT("sid-1234567890"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthChangeWhileParkedTest, "VaRest.Http.Auth.AuthChangeWhileParked",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthChangeWhileParkedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 16 review M-5 (ruled), carried here: WithAuth while requests are parked behind a refresh does not end it
	// (VaRestClient.h:836-839). The refresh belongs to the session; the parked requests keep their Send()-time auth (spec
	// 3.1), which started it (Park joins with Run->Auth, VaRestAuthGate.cpp:213) and applies their replay (AuthApply reads
	// the snapshot's, VaRestAuth.cpp:186). Ending it Failed would fail them for nothing. A request sent after the change
	// carries the new auth's credentials, so the old auth's on the replays is the snapshot's doing, not a change ignored.
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
	if (!TestTrue(TEXT("a client state"), State.IsValid()))
	{
		return false;
	}
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		return Request.Headers.Get(TEXT("Authorization")) == TEXT("Bearer new-access-123456") || Request.Headers.Get(TEXT("X-Api-Key")) == TEXT("other-key-123456")
			? Json(200) : Json(401);
	});
	const TSharedRef<TArray<EVaRestSessionChange>, ESPMode::ThreadSafe> Changes = MakeShared<TArray<EVaRestSessionChange>, ESPMode::ThreadSafe>();
	Rig.Client.OnSessionChanged().AddLambda([Changes](EVaRestSessionChange Change) { Changes->Add(Change); });
	auto A = MakeOutcome<FVaRestJson>();
	auto B = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), A).Send();
	Observe(Rig.Client.Get(TEXT("/b")), B).Send();
	if (!TestTrue(TEXT("both parked on their 401s behind one refresh"), PumpUntil([&Rig, &State] { return Rig.Fake->NumCalls() == 2 && State->NumRefreshWaitersInternal() == 2; })
		&& Rig.Drain() && Auth->IsPending() && Auth->Refreshes == 1))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Client.WithAuth(FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Header, TEXT("X-Api-Key"), TEXT("other-key-123456")));
	Rig.Drain();
	TestTrue(TEXT("the auth change does not end the refresh: still running, both still waiting on it"), State->IsRefreshing() && State->NumRefreshWaitersInternal() == 2
		&& Auth->IsPending() && !A->IsDone() && !B->IsDone() && Rig.Fake->NumCalls() == 2);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	if (!TestTrue(TEXT("both complete"), PumpUntil([A, B] { return A->IsDone() && B->IsDone(); }) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("the refresh ends normally: the session refreshed, announced once, one refresh"), Rig.Client.GetSession().AccessToken.Equals(TEXT("new-access-123456"), ESearchCase::CaseSensitive)
		&& Changes->Num() == 1 && (*Changes)[0] == EVaRestSessionChange::Refreshed && Auth->Refreshes == 1);
	TestTrue(TEXT("both succeed on their replay"), A->SuccessCalls.load() == 1 && A->Response.Attempts == 2 && B->SuccessCalls.load() == 1 && B->Response.Attempts == 2);
	for (const TCHAR* Path : { TEXT("/a"), TEXT("/b") })
	{
		const int32 Replay = FindCall(Rig, Path, 1);
		if (!TestTrue(FString::Printf(TEXT("%s: replayed"), Path), Replay != INDEX_NONE))
		{
			continue;
		}
		const FVaRestHeaders& Headers = Rig.Fake->GetCall(Replay).Request.Headers;
		TestTrue(FString::Printf(TEXT("%s: the replay has the refreshed token, applied by its Send()-time auth, not the new one"), Path),
			Headers.Get(TEXT("Authorization")) == TEXT("Bearer new-access-123456") && !Headers.Contains(TEXT("X-Api-Key")));
	}
	auto C = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/c")), C).Send();
	if (!TestTrue(TEXT("a request sent after the change completes"), Rig.Wait(C) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	const int32 Later = FindCall(Rig, TEXT("/c"));
	if (!TestTrue(TEXT("... sent once"), Later != INDEX_NONE && FindCall(Rig, TEXT("/c"), 1) == INDEX_NONE))
	{
		return false;
	}
	const FVaRestHeaders& LaterHeaders = Rig.Fake->GetCall(Later).Request.Headers;
	TestTrue(TEXT("... with the new auth's credentials (spec 3.1): the API key, no Authorization"), C->SuccessCalls.load() == 1
		&& LaterHeaders.Get(TEXT("X-Api-Key")) == TEXT("other-key-123456") && !LaterHeaders.Contains(TEXT("Authorization")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenRefreshTest, "VaRest.Http.Trace.Golden.RefreshParked",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenRefreshTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// IN-17: 401 -> refresh -> replay with two parked requests (GapBefore, AuthWait, AuthRefreshId). A golden of Task 17's
	// gate: a failure here is a trace point of the gate (or of the attempt boundary it feeds) that moved.
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto First = MakeOutcome<FVaRestJson>();
	const uint64 A = Observe(Rig.Client.Get(TEXT("/a")), First).Send().GetId();
	if (!TestTrue(TEXT("A parked"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	auto Second = MakeOutcome<FVaRestJson>();
	auto Third = MakeOutcome<FVaRestJson>();
	const uint64 B = Observe(Rig.Client.Get(TEXT("/b")), Second).Send().GetId();
	const uint64 C = Observe(Rig.Client.Get(TEXT("/c")), Third).Send().GetId();
	Rig.Drain();
	Rig.Clock->Advance(0.4);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	if (!TestTrue(TEXT("all complete"), PumpUntil([First, Second, Third] { return First->IsDone() && Second->IsDone() && Third->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	FlushRedactionPipe();
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	TestEqualSensitive(TEXT("A: 401, then the replay"), Recorder.KindsOf(A), FString(TEXT("Started,Dispatched,Ended,Dispatched,Ended,Completed")));
	TestEqualSensitive(TEXT("B: one attempt"), Recorder.KindsOf(B), FString(TEXT("Started,Dispatched,Ended,Completed")));
	TestEqualSensitive(TEXT("C: one attempt"), Recorder.KindsOf(C), FString(TEXT("Started,Dispatched,Ended,Completed")));
	TestEqual(TEXT("one refresh for the three"), Auth->Refreshes, 1);
	const TArray<FVaRestTraceAttemptDispatched> ForA = DispatchesOf(Recorder, A);
	if (!TestEqual(TEXT("A dispatched twice"), ForA.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("A's first attempt: at 0 s, no gap, no AuthWait, no refresh"), ForA[0].Attempt == 1 && ForA[0].AttemptStart == 0.0 && ForA[0].GapKind == EVaRestTraceGap::None
		&& ForA[0].AuthWait == 0.0 && ForA[0].AuthRefreshId == 0);
	// Spec review I-1 (Inspector 3.3): a replay's park is the Gap before its attempt, not inside it -- no AuthWait; a park
	// before the first attempt is inside it (Auth), and the attempt starts when it parked
	TestTrue(TEXT("A's replay: attempt 2 at 0.4 s, GapBefore SessionRefresh 0.4 s, no AuthWait, the refresh's id"), ForA[1].Attempt == 2 && ForA[1].AttemptStart == 0.4
		&& ForA[1].GapKind == EVaRestTraceGap::SessionRefresh && FMath::IsNearlyEqual(ForA[1].GapSeconds, 0.4) && ForA[1].AuthWait == 0.0
		&& ForA[1].AuthRefreshId == Auth->LastRefreshId);
	TestTrue(TEXT("... the refresh has an id"), Auth->LastRefreshId != 0);
	for (const uint64 Id : { B, C })
	{
		const TArray<FVaRestTraceAttemptDispatched> Parked = DispatchesOf(Recorder, Id);
		TestTrue(FString::Printf(TEXT("#%llu: attempt 1 started at 0 s when it parked, no gap, AuthWait 0.4 s, the same refresh"), Id), Parked.Num() == 1 && Parked[0].Attempt == 1
			&& Parked[0].AttemptStart == 0.0 && Parked[0].GapKind == EVaRestTraceGap::None && FMath::IsNearlyEqual(Parked[0].AuthWait, 0.4)
			&& Parked[0].AuthRefreshId == Auth->LastRefreshId);
	}
	for (const uint64 Id : { A, B, C })
	{
		FString Why;
		TestTrue(FString::Printf(TEXT("#%llu: Inspector 3.3's phases hold"), Id), PhasesHold(Recorder, Id, Why));
		TestEqualSensitive(FString::Printf(TEXT("#%llu: ... (why not)"), Id), Why, FString());
		const TArray<FVaRestTraceEvent> Events = Recorder.For(Id);
		const FVaRestTraceEvent* Done = Events.FindByPredicate([](const FVaRestTraceEvent& Event) { return Event.Kind == EVaRestTraceKind::Completed; });
		TestTrue(FString::Printf(TEXT("#%llu: Completed at 0.4 s, a success"), Id), Done && Done->Time == 0.4
			&& Done->Payload.Get<FVaRestTraceCompleted>().Outcome == EVaRestTraceOutcome::Success);
	}
	const TArray<FVaRestTraceMarker> Markers = Recorder.MarkersOf(Rig.Client.GetId());
	TestTrue(TEXT("IN-9: SessionRefreshed caused by A"), Markers.ContainsByPredicate([A](const FVaRestTraceMarker& Marker)
	{
		return Marker.Kind == EVaRestTraceMarkerKind::SessionRefreshed && Marker.CauseRequestId == A;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenProactiveTest, "VaRest.Http.Trace.Golden.ProactiveRefresh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenProactiveTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// IN-17 proactive refresh (Task 17): a known expiry inside the lead parks the request before its first attempt
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 10.0);
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const uint64 Id = Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send().GetId();
	Rig.Drain();
	if (!TestTrue(TEXT("parked before its first attempt"), Rig.Fake->NumCalls() == 0 && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.3);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456"), FString(), 3600.0));
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	FlushRedactionPipe();
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	TestEqualSensitive(TEXT("one attempt, after the refresh"), Recorder.KindsOf(Id), FString(TEXT("Started,Dispatched,Ended,Completed")));
	const TArray<FVaRestTraceAttemptDispatched> Dispatched = DispatchesOf(Recorder, Id);
	TestTrue(TEXT("attempt 1 started at 0 s when it parked (spec review I-1), AuthWait 0.3 s, no gap"), Dispatched.Num() == 1 && Dispatched[0].AttemptStart == 0.0
		&& FMath::IsNearlyEqual(Dispatched[0].AuthWait, 0.3) && Dispatched[0].GapKind == EVaRestTraceGap::None && Dispatched[0].AuthRefreshId == Auth->LastRefreshId);
	TestTrue(TEXT("... the refresh has an id"), Auth->LastRefreshId != 0);
	TestTrue(TEXT("... with the new token, ended 200"), Rig.Fake->NumCalls() == 1 && Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 1);
	FString Why;
	TestTrue(TEXT("Inspector 3.3's phases hold"), PhasesHold(Recorder, Id, Why));
	TestEqualSensitive(TEXT("... (why not)"), Why, FString());
	TestTrue(TEXT("IN-9: SessionRefreshed caused by the request"), Recorder.MarkersOf(Rig.Client.GetId()).ContainsByPredicate([Id](const FVaRestTraceMarker& Marker)
	{
		return Marker.Kind == EVaRestTraceMarkerKind::SessionRefreshed && Marker.CauseRequestId == Id;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenParkFailedTest, "VaRest.Http.Trace.Golden.ParkedRefreshFailed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenParkFailedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// M4a final review C-I-1 (ruling, option b), Review Focus line 2: a request parks before its first attempt behind a
	// refresh another request's 401 started, the refresh fails on the network (Failed(Connection), retryable) and Retry is
	// on. The park took attempt 1's number: the note is about attempt 1, the retry is attempt 2 with its backoff gap,
	// Error.Attempts is 2, and Completed names the refresh -- attempt 1 itself has no Dispatched/Ended pair (it never left).
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Url.EndsWith(TEXT("/first")) ? Json(401) : Json(503); });
	FVaRestRetryPolicy Retry = FVaRestRetryPolicy::Exponential(1);
	Retry.Backoff.Jitter = EVaRestJitter::None;
	Rig.Client.WithRetry(Retry);
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto First = MakeOutcome<FVaRestJson>();
	const uint64 A = Observe(Rig.Client.Get(TEXT("/first")).NoRetry(), First).Send().GetId();
	if (!TestTrue(TEXT("A's 401 started a refresh"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	auto Outcome = MakeOutcome<FVaRestJson>();
	const uint64 Id = Observe(Rig.Client.Get(TEXT("/target")), Outcome).Send().GetId();
	Rig.Drain();
	if (!TestEqual(TEXT("B parked before its first attempt"), Rig.Fake->NumCalls(), 1))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.25);
	Auth->Answer(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Connection, TEXT("the refresh endpoint did not answer")));
	if (!TestTrue(TEXT("A fails; B backs off"), PumpUntil([&Rig, First] { return First->IsDone() && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("B completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	TestEqualSensitive(TEXT("the sequence: attempt 1 ended in the park, unsent"), Recorder.KindsOf(Id), FString(TEXT("Started,Note,Dispatched,Ended,Completed")));
	TestTrue(TEXT("Error.Attempts counts the parked attempt"), Outcome->Error.Kind == EVaRestErrorKind::Http && Outcome->Error.HttpStatus == 503
		&& Outcome->Error.Attempts == 2);
	TestTrue(TEXT("... the refresh has an id"), Auth->LastRefreshId != 0);
	const TArray<FVaRestTraceEvent> Events = Recorder.For(Id);
	const FVaRestTraceEvent* Note = Events.FindByPredicate([](const FVaRestTraceEvent& Event) { return Event.Kind == EVaRestTraceKind::Note; });
	TestTrue(TEXT("the note at 0.25 s: attempt 1 ended (Connection), retry in 0.5 s"), Note && Note->Time == 0.25
		&& Note->Payload.Get<FVaRestTraceNote>().Attempt == 1 && Note->Payload.Get<FVaRestTraceNote>().Kind == EVaRestTraceNoteKind::RetryScheduled
		&& Note->Payload.Get<FVaRestTraceNote>().Reason == FName(TEXT("Connection")) && Note->Payload.Get<FVaRestTraceNote>().Delay == 0.5);
	const TArray<FVaRestTraceAttemptDispatched> Dispatched = DispatchesOf(Recorder, Id);
	TestTrue(TEXT("attempt 2 at 0.75 s: GapBefore Backoff 0.5 s from the park's end, no AuthWait, no refresh"), Dispatched.Num() == 1
		&& Dispatched[0].Attempt == 2 && Dispatched[0].AttemptStart == 0.75 && Dispatched[0].GapKind == EVaRestTraceGap::Backoff
		&& FMath::IsNearlyEqual(Dispatched[0].GapSeconds, 0.5) && Dispatched[0].AuthWait == 0.0 && Dispatched[0].AuthRefreshId == 0);
	const FVaRestTraceEvent* Ended = Events.FindByPredicate([](const FVaRestTraceEvent& Event) { return Event.Kind == EVaRestTraceKind::AttemptEnded; });
	TestTrue(TEXT("... ended 503"), Ended && Ended->Payload.Get<FVaRestTraceAttemptEnded>().Attempt == 2 && Ended->Payload.Get<FVaRestTraceAttemptEnded>().Status == 503);
	const FVaRestTraceEvent* Done = Events.FindByPredicate([](const FVaRestTraceEvent& Event) { return Event.Kind == EVaRestTraceKind::Completed; });
	TestTrue(TEXT("Completed: the 503, naming the refresh the park waited on"), Done && Done->Payload.Get<FVaRestTraceCompleted>().Outcome == EVaRestTraceOutcome::Error
		&& Done->Payload.Get<FVaRestTraceCompleted>().AuthRefreshId == Auth->LastRefreshId);
	const TArray<FVaRestTraceEvent> ForA = Recorder.For(A);
	const FVaRestTraceEvent* DoneA = ForA.FindByPredicate([](const FVaRestTraceEvent& Event) { return Event.Kind == EVaRestTraceKind::Completed; });
	TestTrue(TEXT("A's Completed names it too (its replay's park)"), DoneA && DoneA->Payload.Get<FVaRestTraceCompleted>().AuthRefreshId == Auth->LastRefreshId);
	FString Why;
	TestTrue(TEXT("Inspector 3.3's phases hold"), PhasesHold(Recorder, Id, Why));
	TestEqualSensitive(TEXT("... (why not)"), Why, FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenParkRejectedTest, "VaRest.Http.Trace.Golden.ParkedRefreshRejected",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenParkRejectedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// M4a final review C-I-1: a request parked before its first attempt (a proactive refresh) whose refresh is rejected
	// ends in the park: no Dispatched/Ended pair, one attempt counted, and Completed names the refresh it waited on
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 10.0);
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const uint64 Id = Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send().GetId();
	Rig.Drain();
	if (!TestTrue(TEXT("parked before its first attempt"), Rig.Fake->NumCalls() == 0 && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Answer(FVaRestAuthRefreshResult::Rejected(TEXT("the refresh token was revoked")));
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	TestEqualSensitive(TEXT("no attempt left: Started, Completed"), Recorder.KindsOf(Id), FString(TEXT("Started,Completed")));
	TestTrue(TEXT("Kind Auth, one attempt counted"), Outcome->Error.Kind == EVaRestErrorKind::Auth && Outcome->Error.Attempts == 1 && Rig.Fake->NumCalls() == 0);
	const TArray<FVaRestTraceEvent> Events = Recorder.For(Id);
	const FVaRestTraceEvent* Done = Events.FindByPredicate([](const FVaRestTraceEvent& Event) { return Event.Kind == EVaRestTraceKind::Completed; });
	TestTrue(TEXT("Completed names the refresh"), Auth->LastRefreshId != 0 && Done && Done->Payload.Get<FVaRestTraceCompleted>().AuthRefreshId == Auth->LastRefreshId);
	return true;
}

#endif
