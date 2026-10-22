// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Tasks/Task.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::SessionTestPrivate
{
	using FChanges = TSharedRef<TArray<EVaRestSessionChange>, ESPMode::ThreadSafe>;

	FChanges Watch(FVaRestClient& Client)
	{
		FChanges Seen = MakeShared<TArray<EVaRestSessionChange>, ESPMode::ThreadSafe>();
		Client.OnSessionChanged().AddLambda([Seen](EVaRestSessionChange Change) { Seen->Add(Change); });
		return Seen;
	}

	FString Scrub(const FVaRestClient& Client, const TCHAR* Text)
	{
		return VaRest::Http::FClientAccess::State(Client)->GetRedactor()->ScrubText(Text);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionLifecycleTest, "VaRest.Http.Session.Lifecycle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::SessionTestPrivate;
	FTestRig Rig;
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	const FChanges Seen = Watch(Rig.Client);
	Rig.Clock->Advance(5.0);
	Rig.Client.SetSession(TEXT("access-one-123456"), TEXT("refresh-one-123456"), 60.0);
	FVaRestSessionData Data = Rig.Client.GetSession();
	TestTrue(TEXT("both tokens"), Data.AccessToken == TEXT("access-one-123456") && Data.RefreshToken == TEXT("refresh-one-123456"));
	TestEqual(TEXT("the expiry on the client's clock"), Data.ExpiresAt, 65.0);
	const uint32 FirstGeneration = Data.Generation;
	TestTrue(TEXT("a login is Replaced (plan decision 5)"), Seen->Num() == 1 && (*Seen)[0] == EVaRestSessionChange::Replaced);

	Rig.Client.SetBearerToken(TEXT("access-two-123456"), /*bSameIdentity*/ true);
	Data = Rig.Client.GetSession();
	TestTrue(TEXT("a token of the same identity is Refreshed"), Seen->Num() == 2 && (*Seen)[1] == EVaRestSessionChange::Refreshed);
	TestTrue(TEXT("... the refresh token kept, the expiry unknown, the generation up"), Data.RefreshToken == TEXT("refresh-one-123456")
		&& Data.ExpiresAt == 0.0 && Data.Generation > FirstGeneration);
	Rig.Client.SetBearerToken(TEXT("access-two-123456"));
	TestEqual(TEXT("the same token again: no event"), Seen->Num(), 2);

	Rig.Client.SetSessionHeader(TEXT("X-Session-ID"), TEXT("session-id-123456"));
	TestEqualSensitive(TEXT("a session header"), Rig.Client.GetSession().Headers.Get(TEXT("X-Session-ID")), FString(TEXT("session-id-123456")));
	TestEqual(TEXT("... no session event for a header"), Seen->Num(), 2);

	Rig.Client.ClearSession();
	Data = Rig.Client.GetSession();
	TestTrue(TEXT("logout: nothing left"), !Data.HasAccessToken() && !Data.HasRefreshToken() && Data.Headers.IsEmpty());
	TestTrue(TEXT("... Cleared"), Seen->Num() == 3 && (*Seen)[2] == EVaRestSessionChange::Cleared);
	Rig.Client.ClearSession();
	TestEqual(TEXT("clearing an empty session: no event"), Seen->Num(), 3);

	TSharedRef<int32, ESPMode::ThreadSafe> Expired = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Rig.Client.OnSessionExpired().AddLambda([Expired] { ++*Expired; });
	Rig.Client.SetSession(TEXT("access-three-12345"), TEXT("refresh-three-1234"));
	Rig.Client.NotifySessionExpired();
	Data = Rig.Client.GetSession();
	TestTrue(TEXT("WS 19 item 2 NotifySessionExpired: tokens dropped, OnSessionExpired once"), *Expired == 1 && !Data.HasAccessToken() && !Data.HasRefreshToken());
	Rig.Client.NotifySessionExpired();
	TestEqual(TEXT("an expired session expires once (concurrency review I-2)"), *Expired, 1);

	const TArray<FVaRestTraceMarker> Markers = Scoped.Recorder->MarkersOf(Rig.Client.GetId());
	TArray<EVaRestTraceMarkerKind> Kinds;
	for (const FVaRestTraceMarker& Marker : Markers)
	{
		Kinds.Add(Marker.Kind);
	}
	const TArray<EVaRestTraceMarkerKind> Expected = { EVaRestTraceMarkerKind::SessionReplaced, EVaRestTraceMarkerKind::SessionRefreshed,
		EVaRestTraceMarkerKind::SessionCleared, EVaRestTraceMarkerKind::SessionReplaced, EVaRestTraceMarkerKind::SessionExpired };
	TestTrue(TEXT("IN-9: one marker per change, in order"), Kinds == Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionSecretsTest, "VaRest.Http.Session.Secrets",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionSecretsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::SessionTestPrivate;
	FTestRig Rig;
	Rig.Client.SetSecretValue(TEXT("Mine"), TEXT("game-own-secret-123"));
	Rig.Client.SetSession(TEXT("access-secret-123456"), TEXT("refresh-secret-123456"));
	Rig.Client.SetSessionHeader(TEXT("X-Game-Ticket"), TEXT("ticket-value-123456"));
	TestEqualSensitive(TEXT("IN-13 layer 3: the tokens and the header value are scrubbed"),
		Scrub(Rig.Client, TEXT("a access-secret-123456 b refresh-secret-123456 c ticket-value-123456")), FString(TEXT("a *** b *** c ***")));
	TestTrue(TEXT("layer 2: the header's name is a key"), VaRest::Http::FClientAccess::State(Rig.Client)->GetRedactor()->GetRules().ShouldRedact(FStringView(TEXT("X-Game-Ticket")), EVaRestRedactKeyKind::Header));
	Rig.Client.SetBearerToken(TEXT("access-rotated-1234"), true);
	TestEqualSensitive(TEXT("a rotated token stays known (Inspector D21)"), Scrub(Rig.Client, TEXT("access-secret-123456 access-rotated-1234")), FString(TEXT("*** ***")));
	// Review M-2: a game value rotated too, so the clear has a previous value of each owner to judge
	Rig.Client.SetSecretValue(TEXT("Mine"), TEXT("game-own-rotated-12"));
	Rig.Client.ClearSession();
	TestEqualSensitive(TEXT("logout forgets the session's values, the rotated one too (Inspector 12)"),
		Scrub(Rig.Client, TEXT("access-secret-123456 access-rotated-1234 refresh-secret-123456 ticket-value-123456")),
		FString(TEXT("access-secret-123456 access-rotated-1234 refresh-secret-123456 ticket-value-123456")));
	TestEqualSensitive(TEXT("... and keeps the game's own, its rotated one too"), Scrub(Rig.Client, TEXT("game-own-secret-123 game-own-rotated-12")), FString(TEXT("*** ***")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionClearSecretValuesTest, "VaRest.Http.Session.ClearSecretValuesKeepsSession",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionClearSecretValuesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::SessionTestPrivate;
	// Review I-1: ClearSecretValues forgets what SetSecretValue set, not the live session's values. A request sent after it
	// still has the token scrubbed, and the session's rotated token keeps its 10 minutes (Inspector D21), then expires.
	TGuardConsoleVariable<int32> LevelGuard(TEXT("VaRest.LogLevel"), -1); // the client's level, whatever the cvar says
	FTestRig Rig;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Basic); // the rig's is None
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{}"))); });
	Rig.Client.SetSecretValue(TEXT("Mine"), TEXT("game-own-secret-123"));
	Rig.Client.SetSession(TEXT("access-first-123456"), TEXT("refresh-kept-123456"));
	Rig.Client.SetBearerToken(TEXT("access-live-1234567"), /*bSameIdentity*/ true);
	Rig.Client.ClearSecretValues();
	TestEqualSensitive(TEXT("the live tokens are still scrubbed"), Scrub(Rig.Client, TEXT("access-live-1234567 refresh-kept-123456")), FString(TEXT("*** ***")));
	TestEqualSensitive(TEXT("... the session's rotated token keeps its 10 minutes"), Scrub(Rig.Client, TEXT("access-first-123456")), FString(TEXT("***")));
	TestEqualSensitive(TEXT("... and the game's own value is forgotten"), Scrub(Rig.Client, TEXT("game-own-secret-123")), FString(TEXT("game-own-secret-123")));
	{
		FLogCapture Capture;
		auto Done = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/s")).Query(TEXT("q"), TEXT("access-live-1234567")), Done).Send();
		if (!TestTrue(TEXT("it completes"), Rig.Wait(Done)))
		{
			return false;
		}
		TestTrue(TEXT("a line logged after ClearSecretValues scrubs the token"), Capture.Contains(TEXT("GET https://api.test/s?q=*** -> 200")));
		TestFalse(TEXT("... and no line holds it"), Capture.Contains(TEXT("access-live-1234567")));
	}
	Rig.Clock->Advance(FVaRestSecretSet::PreviousLifetime + 1.0);
	TestEqualSensitive(TEXT("the rotated token still expires: ClearSecretValues scheduled its expiry again"), Scrub(Rig.Client, TEXT("access-first-123456")),
		FString(TEXT("access-first-123456")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionGameSlotsTest, "VaRest.Http.Session.GameSlotsApart",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionGameSlotsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::SessionTestPrivate;
	// Review M-1: a game's slots named like the session's own -- one under its prefix, one equal to its access-token slot
	FTestRig Rig;
	Rig.Client.SetSecretValue(TEXT("VaRest.Session.Access"), TEXT("game-named-like-1234"));
	Rig.Client.SetSecretValue(TEXT("VaRest.Session.AccessToken"), TEXT("game-exact-name-1234"));
	Rig.Client.SetSession(TEXT("access-apart-123456"), TEXT("refresh-apart-123456"));
	// Had the session's publish replaced the game's value, it would now be a previous value, and gone after its 10 minutes
	Rig.Clock->Advance(FVaRestSecretSet::PreviousLifetime + 1.0);
	TestEqualSensitive(TEXT("the session's publish does not clobber the game's slot"), Scrub(Rig.Client, TEXT("game-exact-name-1234 access-apart-123456")),
		FString(TEXT("*** ***")));
	Rig.Client.ClearSession();
	TestEqualSensitive(TEXT("ClearSession keeps both game slots"), Scrub(Rig.Client, TEXT("game-named-like-1234 game-exact-name-1234")), FString(TEXT("*** ***")));
	TestEqualSensitive(TEXT("... and forgets the session's"), Scrub(Rig.Client, TEXT("access-apart-123456")), FString(TEXT("access-apart-123456")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionClearAfterExpiryTest, "VaRest.Http.Session.ClearAfterExpiry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionClearAfterExpiryTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::SessionTestPrivate;
	// Review M-3: a logout after an expiry. The expiry retired the tokens (still known for 10 minutes, Inspector D21); the
	// logout forgets them (Inspector 12) without a Cleared for a session that has nothing left.
	FTestRig Rig;
	const FChanges Seen = Watch(Rig.Client);
	TSharedRef<int32, ESPMode::ThreadSafe> Expired = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Rig.Client.OnSessionExpired().AddLambda([Expired] { ++*Expired; });
	Rig.Client.SetSession(TEXT("access-expired-12345"), TEXT("refresh-expired-1234"));
	Rig.Client.NotifySessionExpired();
	TestEqualSensitive(TEXT("the expiry retires the tokens: still known"), Scrub(Rig.Client, TEXT("access-expired-12345 refresh-expired-1234")), FString(TEXT("*** ***")));
	const uint32 Generation = Rig.Client.GetSession().Generation;
	Rig.Client.ClearSession();
	TestEqualSensitive(TEXT("the logout forgets them"), Scrub(Rig.Client, TEXT("access-expired-12345 refresh-expired-1234")),
		FString(TEXT("access-expired-12345 refresh-expired-1234")));
	TestTrue(TEXT("... with no Cleared"), Seen->Num() == 1 && (*Seen)[0] == EVaRestSessionChange::Replaced);
	TestEqual(TEXT("... no second expiry"), *Expired, 1);
	TestEqual(TEXT("... and the session data unchanged"), Rig.Client.GetSession().Generation, Generation);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionEmptySetSessionTest, "VaRest.Http.Session.EmptySetSessionClears",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionEmptySetSessionTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::SessionTestPrivate;
	// Review M-4: SetSession with no token is ClearSession, as SetBearerToken("") is -- not a login of another identity
	FTestRig Rig;
	const FChanges Seen = Watch(Rig.Client);
	Rig.Client.SetSession(TEXT("access-empty-123456"), TEXT("refresh-empty-123456"));
	Rig.Client.SetSessionHeader(TEXT("X-Session-ID"), TEXT("session-id-123456"));
	Rig.Client.SetSession(TEXT(""), TEXT(""));
	TestTrue(TEXT("Replaced, then Cleared"), Seen->Num() == 2 && (*Seen)[0] == EVaRestSessionChange::Replaced && (*Seen)[1] == EVaRestSessionChange::Cleared);
	const FVaRestSessionData Data = Rig.Client.GetSession();
	TestTrue(TEXT("... nothing left, the session header included"), !Data.HasAccessToken() && !Data.HasRefreshToken() && Data.Headers.IsEmpty());
	Rig.Client.SetSession(TEXT(""), TEXT(""));
	TestEqual(TEXT("again, on an empty session: no event"), Seen->Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionUnchangedHeaderTest, "VaRest.Http.Session.UnchangedHeader",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionUnchangedHeaderTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::SessionTestPrivate;
	// Review M-5: setting a session header to the value it has changes nothing -- no generation, no new redactor, no event
	FTestRig Rig;
	const FChanges Seen = Watch(Rig.Client);
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
	if (!TestTrue(TEXT("a client state"), State.IsValid()))
	{
		return false;
	}
	Rig.Client.SetSessionHeader(TEXT("X-Session-ID"), TEXT("session-id-123456"));
	const uint32 Generation = Rig.Client.GetSession().Generation;
	// Held, so its address cannot be reused by the redactor a publish would make
	const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Before = State->GetRedactor();
	Rig.Client.SetSessionHeader(TEXT("X-Session-ID"), TEXT("session-id-123456"));
	TestEqual(TEXT("the same value: the generation stays"), Rig.Client.GetSession().Generation, Generation);
	TestTrue(TEXT("... and nothing is published"), &State->GetRedactor().Get() == &Before.Get());
	Rig.Client.SetSessionHeader(TEXT("X-Session-ID"), TEXT("session-id-654321"));
	TestTrue(TEXT("a new value: the generation moves and a redactor is published"), Rig.Client.GetSession().Generation > Generation
		&& &State->GetRedactor().Get() != &Before.Get());
	TestEqual(TEXT("no session event for a header"), Seen->Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionRefusesTest, "VaRest.Http.Session.RefusesControlCharacters",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionRefusesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Decision 11's rule for credentials: a token or a session header goes into a header line
	AddExpectedErrorPlain(TEXT("VaRest: SetBearerToken: the token holds"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("VaRest: SetSessionHeader: the name is not a token"), EAutomationExpectedErrorFlags::Contains, 1);
	FTestRig Rig;
	Rig.Client.SetBearerToken(TEXT("abc\r\nX-Evil: 1"));
	Rig.Client.SetSessionHeader(TEXT("Bad Name"), TEXT("v"));
	TestFalse(TEXT("no token set"), Rig.Client.GetSession().HasAccessToken());
	TestTrue(TEXT("no header set"), Rig.Client.GetSession().Headers.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionDropsClientTest, "VaRest.Http.Session.HandlerDropsClient",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionDropsClientTest::RunTest(const FString& Parameters)
{
	// Concurrency review C-1: "a fresh client on logout" -- a listener reassigns the only FVaRestClient while the client
	// state is delivering to it. RunSessionChange keeps the state alive until the change returns; a weak probe shows it
	// alive inside each listener and gone after. Task 17 adds the parked-request half (VaRest.Http.Auth.HandlerDropsClientWhileParked).
	using FClientHolder = TSharedRef<FVaRestClient, ESPMode::ThreadSafe>;
	using FStateWeak = TWeakPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe>;
	using FAlive = TSharedRef<TArray<bool>, ESPMode::ThreadSafe>;
	for (int32 Case = 0; Case < 3; ++Case)
	{
		const FClientHolder Holder = MakeShared<FVaRestClient, ESPMode::ThreadSafe>(FVaRestClient::Make(TEXT("https://api.test")));
		if (Case > 0)
		{
			// A session to clear or to expire, set before the listener is bound
			Holder->SetSession(TEXT("token-dropped-123456"), TEXT("refresh-dropped-12345"));
		}
		const FStateWeak Weak = VaRest::Http::FClientAccess::State(*Holder);
		const FAlive Alive = MakeShared<TArray<bool>, ESPMode::ThreadSafe>();
		// Copied out first: without the keep-alive, the assignment frees this lambda with the delegate that holds it
		auto Drop = [Holder, Weak, Alive]
		{
			const FClientHolder Owner = Holder;
			const FStateWeak Watched = Weak;
			const FAlive Into = Alive;
			*Owner = FVaRestClient();
			Into->Add(Watched.IsValid());
		};
		const TCHAR* Name = Case == 0 ? TEXT("SetBearerToken (Replaced)") : Case == 1 ? TEXT("ClearSession (Cleared)") : TEXT("NotifySessionExpired (OnSessionExpired)");
		if (Case < 2)
		{
			Holder->OnSessionChanged().AddLambda([Drop](EVaRestSessionChange) { Drop(); });
		}
		else
		{
			Holder->OnSessionExpired().AddLambda([Drop] { Drop(); });
		}
		FVaRestClient& Client = *Holder;
		if (Case == 0)
		{
			Client.SetBearerToken(TEXT("token-replaced-123456"));
		}
		else if (Case == 1)
		{
			Client.ClearSession();
		}
		else
		{
			Client.NotifySessionExpired();
		}
		TestTrue(FString::Printf(TEXT("%s: the listener ran once and saw the state alive"), Name), Alive->Num() == 1 && (*Alive)[0]);
		TestFalse(FString::Printf(TEXT("%s: ... and the state went when the call returned"), Name), Weak.IsValid());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionNestedTest, "VaRest.Http.Session.NestedChangeOrder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionNestedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Concurrency review I-2: a listener that changes the session inside an event (a game rejecting a token) must not make
	// a later listener see the events out of order -- W5's socket manager restarts sockets on Replaced and closes them on
	// Cleared. UE broadcasts in reverse binding order (MulticastDelegateBase.h:299-300): L2 is bound first to run second.
	const TSharedRef<FVaRestClient, ESPMode::ThreadSafe> Holder = MakeShared<FVaRestClient, ESPMode::ThreadSafe>(FVaRestClient::Make(TEXT("https://api.test")));
	const TSharedRef<TArray<EVaRestSessionChange>, ESPMode::ThreadSafe> SeenByL2 = MakeShared<TArray<EVaRestSessionChange>, ESPMode::ThreadSafe>();
	Holder->OnSessionChanged().AddLambda([SeenByL2](EVaRestSessionChange Change) { SeenByL2->Add(Change); });
	const TWeakPtr<FVaRestClient, ESPMode::ThreadSafe> WeakClient = Holder;
	Holder->OnSessionChanged().AddLambda([WeakClient](EVaRestSessionChange Change)
	{
		const TSharedPtr<FVaRestClient, ESPMode::ThreadSafe> Pinned = WeakClient.Pin();
		if (Pinned.IsValid() && Change == EVaRestSessionChange::Replaced)
		{
			Pinned->ClearSession();
		}
	});
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	Holder->SetBearerToken(TEXT("token-rejected-123456"));
	TestTrue(TEXT("every listener sees Replaced, then Cleared"), SeenByL2->Num() == 2 && (*SeenByL2)[0] == EVaRestSessionChange::Replaced
		&& (*SeenByL2)[1] == EVaRestSessionChange::Cleared);
	TestFalse(TEXT("... and the session is the last change's: empty"), Holder->GetSession().HasAccessToken());
	TArray<EVaRestTraceMarkerKind> Kinds;
	for (const FVaRestTraceMarker& Marker : Scoped.Recorder->MarkersOf(Holder->GetId()))
	{
		Kinds.Add(Marker.Kind);
	}
	TestTrue(TEXT("IN-9: the markers in the same order"), Kinds == TArray<EVaRestTraceMarkerKind>{ EVaRestTraceMarkerKind::SessionReplaced, EVaRestTraceMarkerKind::SessionCleared });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionListenersTest, "VaRest.Http.Session.ListenersDieOnGameThread",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionListenersTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Concurrency review M-9: a client whose last handle a game's worker drops dies there; the I-1 backstop sends its
	// configuration to the game thread, and the session's listeners -- game code and what it captures -- go with it
	struct FWhereDestroyed
	{
		explicit FWhereDestroyed(TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> InInto)
			: Into(MoveTemp(InInto))
		{
		}
		~FWhereDestroyed() { Into->store(IsInGameThread() ? 1 : 0); }
		TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Into;
	};
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Where = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(-1);
	FVaRestClient Doomed = FVaRestClient::Make(TEXT("https://worker.test"));
	// The listener holds the probe's only reference
	Doomed.OnSessionChanged().AddLambda([Probe = MakeShared<FWhereDestroyed, ESPMode::ThreadSafe>(Where)](EVaRestSessionChange) {});
	const UE::Tasks::FTask Dropping = UE::Tasks::Launch(TEXT("VaRest.Test.DropClient"), [Last = MoveTemp(Doomed)]() mutable
	{
		Last = FVaRestClient();
	});
	// Not Wait(): it may run the task here, on the game thread (Task.h:65-80); as VaRest.Http.Trace.ClientDestroyedOffGameThread
	const double Deadline = FPlatformTime::Seconds() + 5.0;
	while (!Dropping.IsCompleted() && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(0.001f);
	}
	if (!TestTrue(TEXT("the worker dropped the last handle"), Dropping.IsCompleted()))
	{
		return false;
	}
	TestTrue(TEXT("the listener is released"), PumpUntil([Where] { return Where->load() != -1; }));
	TestEqual(TEXT("... on the game thread"), Where->load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionExportTest, "VaRest.Http.Session.ExportImport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionExportTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::SessionTestPrivate;
	FTestRig From;
	From.Client.SetSession(TEXT("access-export-12345"), TEXT("refresh-export-1234"), 120.0);
	From.Client.SetSessionHeader(TEXT("X-Session-ID"), TEXT("session-export-123"));
	const FString Exported = From.Client.ExportSession();
	TestTrue(TEXT("an export holds the tokens (the docs carry the caveat)"), Exported.Contains(TEXT("access-export-12345")) && Exported.Contains(TEXT("\"version\":1")));

	FTestRig To;
	const FChanges Seen = Watch(To.Client);
	if (!TestTrue(TEXT("imported"), To.Client.ImportSession(Exported)))
	{
		return false;
	}
	const FVaRestSessionData Data = To.Client.GetSession();
	TestTrue(TEXT("the same tokens and header"), Data.AccessToken == TEXT("access-export-12345") && Data.RefreshToken == TEXT("refresh-export-1234")
		&& Data.Headers.Get(TEXT("X-Session-ID")) == TEXT("session-export-123"));
	TestTrue(TEXT("the expiry carried through UTC, within a second"), FMath::Abs(Data.ExpiresAt - 120.0) < 1.0);
	TestTrue(TEXT("an import is Replaced"), Seen->Num() == 1 && (*Seen)[0] == EVaRestSessionChange::Replaced);
	TestEqualSensitive(TEXT("... and its values are secrets"), Scrub(To.Client, TEXT("session-export-123")), FString(TEXT("***")));

	FTestRig Bad;
	TestFalse(TEXT("not JSON"), Bad.Client.ImportSession(TEXT("{nope")));
	TestFalse(TEXT("another version"), Bad.Client.ImportSession(TEXT(R"({"version":2,"accessToken":"a"})")));
	TestFalse(TEXT("a token with CR LF"), Bad.Client.ImportSession(TEXT(R"({"version":1,"accessToken":"a\r\nX-Evil: 1"})")));
	TestFalse(TEXT("a bad date"), Bad.Client.ImportSession(TEXT(R"({"version":1,"accessToken":"a","expiresAt":"tomorrow"})")));
	// Spec review M-12: the one game-thread parse is bounded -- valid JSON padded past 64 Ki characters is refused unparsed
	const FString Padded = FString(TEXT(R"({"version":1,"accessToken":"padded-token-123456"})")) + FString::ChrN(65 * 1024, TEXT(' '));
	TestFalse(TEXT("over 64 KiB"), Bad.Client.ImportSession(Padded));
	TestFalse(TEXT("nothing changed"), Bad.Client.GetSession().HasAccessToken());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionImportEmptyTest, "VaRest.Http.Session.ImportEmptyIsLogout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionImportEmptyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::SessionTestPrivate;
	// Controller ruling (Task 13), consistent with review M-4: an import with no token is a logout, and an empty session's
	// export imports as an empty session
	FTestRig Rig;
	const FChanges Seen = Watch(Rig.Client);
	Rig.Client.SetSession(TEXT("access-gone-1234567"), TEXT("refresh-gone-123456"));
	Rig.Client.SetSessionHeader(TEXT("X-Session-ID"), TEXT("session-gone-12345"));
	TestTrue(TEXT("an import with both tokens empty is accepted"),
		Rig.Client.ImportSession(TEXT(R"({"version":1,"accessToken":"","refreshToken":"","headers":[{"name":"X-Other","value":"other-value-123456"}]})")));
	const FVaRestSessionData Data = Rig.Client.GetSession();
	TestTrue(TEXT("... nothing left, the imported header ignored"), !Data.HasAccessToken() && !Data.HasRefreshToken() && Data.Headers.IsEmpty());
	TestTrue(TEXT("... Cleared, not Replaced"), Seen->Num() == 2 && (*Seen)[0] == EVaRestSessionChange::Replaced && (*Seen)[1] == EVaRestSessionChange::Cleared);
	TestEqualSensitive(TEXT("... the session's secrets forgotten"), Scrub(Rig.Client, TEXT("access-gone-1234567 session-gone-12345")),
		FString(TEXT("access-gone-1234567 session-gone-12345")));
	TestTrue(TEXT("the empty session's own export imports as empty, with no event"), Rig.Client.ImportSession(Rig.Client.ExportSession()) && Seen->Num() == 2);
	TestTrue(TEXT("... and still empty"), !Rig.Client.GetSession().HasAccessToken() && !Rig.Client.GetSession().HasRefreshToken());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionImportRedactsTest, "VaRest.Http.Session.ImportRedactsHeaders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionImportRedactsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::SessionTestPrivate;
	// IN-13 layers 2 and 3 for an import, with nothing sent: the name is a redaction key, the value a session slot
	FTestRig Rig;
	Rig.Client.SetSessionHeader(TEXT("X-Old-Header"), TEXT("old-header-value-12"));
	TestTrue(TEXT("imported"), Rig.Client.ImportSession(
		TEXT(R"({"version":1,"accessToken":"access-import-1234567","refreshToken":"","unknown":[1],"headers":[{"name":"X-Imported-Id","value":"imported-value-1234"}]})")));
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
	if (!TestTrue(TEXT("a client state"), State.IsValid()))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the imported header's value is scrubbed"), Scrub(Rig.Client, TEXT("imported-value-1234")), FString(TEXT("***")));
	TestEqualSensitive(TEXT("... the token too"), Scrub(Rig.Client, TEXT("access-import-1234567")), FString(TEXT("***")));
	TestTrue(TEXT("... and the name is a redaction key"), State->GetConfig()->RedactedKeys.ContainsByPredicate(
		[](const FString& Key) { return Key.Equals(TEXT("X-Imported-Id"), ESearchCase::IgnoreCase); }));
	TestTrue(TEXT("the replaced header is gone from the session"), !Rig.Client.GetSession().Headers.Find(TEXT("X-Old-Header")));
	TestEqualSensitive(TEXT("... its value retired, still known"), Scrub(Rig.Client, TEXT("old-header-value-12")), FString(TEXT("***")));
	// Retired, not kept forever: only a retired slot is scheduled to expire (review M-2; without the retire the stale value stays)
	Rig.Clock->Advance(FVaRestSecretSet::PreviousLifetime + 1.0);
	TestEqualSensitive(TEXT("... and forgotten after its 10 minutes"), Scrub(Rig.Client, TEXT("old-header-value-12")), FString(TEXT("old-header-value-12")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionImportRefusesTest, "VaRest.Http.Session.ImportRefusesMalformed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionImportRefusesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::SessionTestPrivate;
	// Review I-1, M-1, M-2: a refused import changes nothing -- above all it is never a logout
	FTestRig Rig;
	Rig.Client.SetSession(TEXT("access-keep-1234567"), TEXT("refresh-keep-123456"), 60.0);
	Rig.Client.SetSessionHeader(TEXT("X-Session-ID"), TEXT("session-keep-12345"));
	const FChanges Seen = Watch(Rig.Client);
	const FVaRestSessionData Before = Rig.Client.GetSession();
	struct FCase { const TCHAR* Name; const TCHAR* Json; };
	const FCase Cases[] = {
		{ TEXT("a token a number"), TEXT(R"({"version":1,"accessToken":5,"refreshToken":""})") },
		{ TEXT("a token null"), TEXT(R"({"version":1,"accessToken":null,"refreshToken":""})") },
		{ TEXT("a token key missing"), TEXT(R"({"version":1,"accessToken":""})") },
		{ TEXT("only the version"), TEXT(R"({"version":1})") },
		{ TEXT("expiresAt a number"), TEXT(R"({"version":1,"accessToken":"a","refreshToken":"","expiresAt":5})") },
		{ TEXT("headers an object"), TEXT(R"({"version":1,"accessToken":"a","refreshToken":"","headers":{}})") },
		{ TEXT("a header entry not an object"), TEXT(R"({"version":1,"accessToken":"a","refreshToken":"","headers":["x"]})") },
		{ TEXT("a header value a number"), TEXT(R"({"version":1,"accessToken":"a","refreshToken":"","headers":[{"name":"X-A","value":5}]})") },
		{ TEXT("a header name twice, any case"), TEXT(R"({"version":1,"accessToken":"a","refreshToken":"","headers":[{"name":"X-A","value":"1"},{"name":"x-a","value":"2"}]})") },
		{ TEXT("a control character in a header value"), TEXT(R"({"version":1,"accessToken":"a","refreshToken":"","headers":[{"name":"X-A","value":"1\r\nX-Evil: 2"}]})") },
		{ TEXT("a control character in a header name"), TEXT(R"({"version":1,"accessToken":"a","refreshToken":"","headers":[{"name":"X-A\n","value":"1"}]})") },
	};
	for (const FCase& Case : Cases)
	{
		TestFalse(FString::Printf(TEXT("refused: %s"), Case.Name), Rig.Client.ImportSession(Case.Json));
		const FVaRestSessionData After = Rig.Client.GetSession();
		TestTrue(FString::Printf(TEXT("... tokens, headers and generation unchanged: %s"), Case.Name), After.AccessToken == Before.AccessToken
			&& After.RefreshToken == Before.RefreshToken && After.ExpiresAt == Before.ExpiresAt && After.Generation == Before.Generation
			&& After.Headers.Num() == 1 && After.Headers.Get(TEXT("X-Session-ID")) == TEXT("session-keep-12345"));
		TestEqualSensitive(FString::Printf(TEXT("... the old token still scrubbed: %s"), Case.Name), Scrub(Rig.Client, TEXT("access-keep-1234567")), FString(TEXT("***")));
	}
	TestEqual(TEXT("no event for any of them"), Seen->Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionImportBoundTest, "VaRest.Http.Session.ImportBoundary",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionImportBoundTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::SessionTestPrivate;
	// Review M-2: exactly MaxSessionJsonChars is accepted, one more is refused unparsed
	FTestRig Rig;
	const FString Head(TEXT(R"({"version":1,"accessToken":"edge-token-1234567","refreshToken":""})"));
	const FString AtLimit = Head + FString::ChrN(VaRest::Http::MaxSessionJsonChars - Head.Len(), TEXT(' '));
	const FString PastLimit = AtLimit + TEXT(" ");
	TestEqual(TEXT("built to the limit"), AtLimit.Len(), VaRest::Http::MaxSessionJsonChars);
	TestFalse(TEXT("one past the limit"), Rig.Client.ImportSession(PastLimit));
	TestFalse(TEXT("... changed nothing"), Rig.Client.GetSession().HasAccessToken());
	TestTrue(TEXT("exactly the limit"), Rig.Client.ImportSession(AtLimit));
	TestTrue(TEXT("... imported"), Rig.Client.GetSession().AccessToken == TEXT("edge-token-1234567"));
	return true;
}

#endif
