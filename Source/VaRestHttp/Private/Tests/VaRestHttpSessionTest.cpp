// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

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
	Rig.Client.ClearSession();
	TestEqualSensitive(TEXT("logout forgets the session's values (Inspector 12)"), Scrub(Rig.Client, TEXT("access-rotated-1234 ticket-value-123456")),
		FString(TEXT("access-rotated-1234 ticket-value-123456")));
	TestEqualSensitive(TEXT("... and keeps the game's own"), Scrub(Rig.Client, TEXT("game-own-secret-123")), FString(TEXT("***")));
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

#endif
