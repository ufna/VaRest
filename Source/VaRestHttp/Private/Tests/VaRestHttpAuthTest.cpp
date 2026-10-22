// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
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
	FTestRig BadHeader;
	BadHeader.Client.WithAuth(FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Header, TEXT("Bad Name"), TEXT("key-value-123456")));
	FTestRig EmptyQuery;
	EmptyQuery.Client.WithAuth(FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Query, TEXT(""), TEXT("key-value-123456")));
	FTestRig BadUser;
	BadUser.Client.WithAuth(FVaRestAuth::Basic(TEXT("a:b"), TEXT("password-123456")));
	FTestRig* const Rigs[] = { &BadHeader, &EmptyQuery, &BadUser };
	const TCHAR* const Names[] = { TEXT("ApiKey header \"Bad Name\""), TEXT("ApiKey query \"\""), TEXT("Basic user \"a:b\"") };
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

#endif
