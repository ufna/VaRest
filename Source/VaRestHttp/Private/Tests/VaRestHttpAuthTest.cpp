// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
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
	const FVaRestAuthRefreshContext Context(Rig.Client, Rig.Client.GetSession(), RefreshId);
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
		Kept = MakeUnique<FVaRestAuthRefreshContext>(Gone, FVaRestSessionData(), FVaRestTrace::NewRequestId());
	}
	TestFalse(TEXT("M-10: the context does not keep its client alive"), GoneState.IsValid());
	// Re-review N-1: Send() on an empty client logs this Error before it delivers Kind=Config (VaRestClient.cpp:148-152)
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

	const FVaRestAuthRefreshContext Context(Rig.Client, Rig.Client.GetSession(), FVaRestTrace::NewRequestId());
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

#endif
