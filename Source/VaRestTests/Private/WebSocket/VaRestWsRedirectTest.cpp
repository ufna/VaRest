// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// Ruling Q2 (VaRest-2rt): an upgrade redirect is never followed -- to the same port, an absolute URL, wss, a downgrade, with
// any redirect status, or to a target that refuses TCP, does not resolve, or hangs

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestHttpTesting.h"
#include "VaRestUrl.h"
#include "VaRestWsTestUtil.h"

namespace VaRest::WsTest::VaRestWsRedirectTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;

	/**
	 * Game thread: /ws/status/<Status> to Location, with an Authorization header; checks the attempt failed Redirect and, when TargetKey is
	 * given, that the target saw nothing
	 */
	void ExpectRefused(FAutomationTestBase& Test, ILwsTestTransport& Transport, const FSite& Site, const FScheme& Scheme, int32 Status, const FString& Location,
		const FString& TargetKey, const TCHAR* What, bool bExpectTlsVeto = false)
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, FString::Printf(TEXT("/ws/status/%d?location=%s&key=%s"), Status, *VaRest::Http::PercentEncode(Location), *Key));
		Req.Headers = { { TEXT("Authorization"), TEXT("Bearer redirect-test-token") } };
		const TUniquePtr<FEvents> Events = Visit(Transport, MoveTemp(Req));
		const FVaRestSocketEvent* End = Events->GetEnd();
		const bool bPrivate = VaRest::WebSocket::Testing::UsesPrivateNumericRoute();
		const int32 ExpectedStatus = bPrivate ? Status : 0;
		Test.TestTrue(FString::Printf(TEXT("%s %s: Error Redirect, actual backend status %d (%s)"), Scheme.Name, What, ExpectedStatus, *Events->Describe()),
			End && End->Type == EVaRestSocketEventType::Error && End->End.Failure == EVaRestSocketFailure::Redirect && End->End.HttpStatus == ExpectedStatus);
		if (bPrivate)
			Test.TestTrue(TEXT("private redirect is a transport refusal before a target hop"), End && End->End.By == EVaRestSocketCloseBy::Transport);
		if (bExpectTlsVeto && !bPrivate)
		{
			Test.TestTrue(FString::Printf(TEXT("%s %s: refused at the redirect TLS handshake (%s)"), Scheme.Name, What, *Events->Describe()),
				End && End->End.Detail.Contains(TEXT("refused at the redirect TLS handshake"), ESearchCase::CaseSensitive));
		}
		Test.TestEqual(FString::Printf(TEXT("%s %s: the %d was asked once"), Scheme.Name, What, Status), FetchJournal(Test, Site, Key)[TEXT("connections")].AsInt(), 1);
		if (!TargetKey.IsEmpty())
		{
			// The target never got a request, so the Authorization header never reached it (ruling Q2)
			Test.TestEqual(FString::Printf(TEXT("%s %s: the target saw no request"), Scheme.Name, What), FetchJournal(Test, Site, TargetKey, 1, 1000)[TEXT("connections")].AsInt(), 0);
		}
	}
}

#define VAREST_WS_TEST_FLAGS (EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
#define VAREST_WS_BEGIN() \
	using namespace VaRest::WsTest; \
	using namespace VaRest::WsTest::VaRestWsRedirectTestPrivate; \
	ExpectLwsLines(*this); \
	FSite Site; \
	if (!FindSite(*this, Site)) { return false; } \
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site); \
	if (!Transport.IsValid()) { return false; } \
	ON_SCOPE_EXIT { Transport->Shutdown(); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsRedirectSameServerTest, "VaRest.Integration.WebSocket.Transport.Redirect.SameServer", VAREST_WS_TEST_FLAGS)

bool FVaRestWsRedirectSameServerTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		// Private numeric backend refuses before a target hop; legacy LWS is refused at TLS verification or the second APPEND.
		const FString Relative = NewKey();
		ExpectRefused(*this, *Transport, Site, Scheme, 302, TEXT("/ws/echo?key=") + Relative, Relative, TEXT("relative"), Scheme.bTls);
		// hs.redirect_absolute: the same server by absolute URL. By IP: LWS resolves a Location's host itself, and localhost is ::1
		// first here (plan decision 19), so a name would fail the TCP connect before the redirect could be seen
		const FString Absolute = NewKey();
		ExpectRefused(*this, *Transport, Site, Scheme, 302, FString::Printf(TEXT("%s://%s:%d/ws/echo?key=%s"), Scheme.bTls ? TEXT("wss") : TEXT("ws"), *Site.Host,
			Scheme.bTls ? Site.TlsPort : Site.Port, *Absolute), Absolute, TEXT("absolute"), Scheme.bTls);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsRedirectSchemeTest, "VaRest.Integration.WebSocket.Transport.Redirect.OtherScheme", VAREST_WS_TEST_FLAGS)

bool FVaRestWsRedirectSchemeTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	const TArray<FScheme> Both = Schemes(Site);
	// hs.redirect_to_tls: ws -> wss to another port (by IP, as W0 did); refused at the certificate check of a ws:// attempt
	const FString ToTls = NewKey();
	ExpectRefused(*this, *Transport, Site, Both[0], 302, FString::Printf(TEXT("wss://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *ToTls), ToTls, TEXT("to wss"), true);
	// hs.redirect_downgrade: wss -> ws, which LWS refuses itself ("HS: Redirect attempted SSL downgrade")
	const FString Down = NewKey();
	ExpectRefused(*this, *Transport, Site, Both[1], 302, FString::Printf(TEXT("ws://%s:%d/ws/echo?key=%s"), *Site.Host, Site.Port, *Down), Down, TEXT("downgrade"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsRedirectStatusesTest, "VaRest.Integration.WebSocket.Transport.Redirect.Statuses", VAREST_WS_TEST_FLAGS)

bool FVaRestWsRedirectStatusesTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	// Every redirect status: private policy rejects the actual parsed 3xx; legacy hooks reject LWS follow-up activity.
	for (const FScheme& Scheme : Schemes(Site))
	{
		for (const int32 Status : { 301, 302, 303, 307, 308 })
		{
			const FString Target = NewKey();
			ExpectRefused(*this, *Transport, Site, Scheme, Status, TEXT("/ws/echo?key=") + Target, Target, *FString::Printf(TEXT("%d relative"), Status));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsRedirectUnreachableTest, "VaRest.Integration.WebSocket.Transport.Redirect.Unreachable", VAREST_WS_TEST_FLAGS)

bool FVaRestWsRedirectUnreachableTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	// Task 10 review M-2: a redirect LWS follows to a target that refuses TCP is still a redirect. Spec 3.4 puts "upgrade 3xx
	// (redirect not followed or failed)" in a stop row; reported as ConnectFailed it would make W2 retry a redirecting server.
	// The target is loopback port 1, where nothing listens (Connect.Refused's target, [hs.connect_refused]): the host's TUN
	// proxy completes every connect that leaves loopback, so no other closed target exists here
	for (const FScheme& Scheme : Schemes(Site))
	{
		ExpectRefused(*this, *Transport, Site, Scheme, 302, FString::Printf(TEXT("%s://127.0.0.1:1/"), Scheme.bTls ? TEXT("wss") : TEXT("ws")), FString(), TEXT("to a closed port"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsRedirectUnresolvableTest, "VaRest.Integration.WebSocket.Transport.Redirect.Unresolvable", VAREST_WS_TEST_FLAGS)

bool FVaRestWsRedirectUnresolvableTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	// Task 11 carry: a redirect LWS follows to a host that does not resolve. LWS 3.0 raises no callback for it at all: after
	// lws_client_reset (the old socket out of the poll set, the state back to unconnected), lws_client_connect_2's getaddrinfo
	// failure frees the wsi without CONNECTION_ERROR or WSI_DESTROY (FConn::ReapIfDropped cites the archive). Spec 3.4: a
	// failed redirect is a stop row, so Redirect, not a hang until ConnectTimeout -- whose kill would reach freed memory.
	// "a..b": glibc's getaddrinfo refuses the empty label itself (EAI_NONAME, no query sent), so the host's TUN proxy, which
	// answers every DNS query and completes every connect that leaves loopback, never sees it; a name a resolver answers
	// NXDOMAIN for fails at the same getaddrinfo call. ConnectTimeout 4 s: an end by the deadline would miss the 2 s asked here
	const int32 InFlightBefore = VaRest::WebSocket::Testing::GetTlsHandshakesInFlight();
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		const FString Location = FString::Printf(TEXT("%s://a..b:1/"), Scheme.bTls ? TEXT("wss") : TEXT("ws"));
		FVaRestSocketConnectRequest Req = Request(Scheme, FString::Printf(TEXT("/ws/status/302?location=%s&key=%s"), *VaRest::Http::PercentEncode(Location), *Key));
		Req.ConnectTimeout = 4.0;
		const double Started = FPlatformTime::Seconds();
		FEvents Events(Transport->Connect(MoveTemp(Req)));
		TestTrue(FString::Printf(TEXT("%s: ended within 2 s"), Scheme.Name), Events.WaitEnd(2.0));
		const FVaRestSocketEvent* End = Events.GetEnd();
		if (!TestNotNull(FString::Printf(TEXT("%s: a terminal event"), Scheme.Name), End))
		{
			Events.Connection->Close(1000, TEXT("done"));
			Events.WaitEnd(30.0);
			continue;
		}
		const int32 ExpectedStatus = VaRest::WebSocket::Testing::UsesPrivateNumericRoute() ? 302 : 0;
		TestTrue(FString::Printf(TEXT("%s: Error Redirect, backend status %d, after %.2f s (%s)"), Scheme.Name, ExpectedStatus, End->Time - Started, *Events.Describe()),
			End->Type == EVaRestSocketEventType::Error && End->End.Failure == EVaRestSocketFailure::Redirect && End->End.HttpStatus == ExpectedStatus);
		TestFalse(FString::Printf(TEXT("%s: the Detail names no host"), Scheme.Name), End->End.Detail.Contains(TEXT("a..b"), ESearchCase::IgnoreCase));
		TestEqual(FString::Printf(TEXT("%s: the 302 was asked once"), Scheme.Name), FetchJournal(*this, Site, Key)[TEXT("connections")].AsInt(), 1);
		// No kill after either private pre-hop refusal or the legacy verified dropped-WSI path.
		TestTrue(FString::Printf(TEXT("%s: the connection left the service"), Scheme.Name),
			VaRest::Http::Testing::PumpUntil([&Transport] { return Transport->GetCounters().LiveConnections == 0; }, 5.0, true));
	}
	TestEqual(TEXT("no kill was issued"), Transport->GetCounters().KillsIssued, 0);
	TestEqual(TEXT("the wss attempt left the handshakes in flight (VaRest-rti)"), VaRest::WebSocket::Testing::GetTlsHandshakesInFlight(), InFlightBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsRedirectHangsAfterRedirectTest, "VaRest.Integration.WebSocket.Transport.Redirect.HangsAfterRedirect", VAREST_WS_TEST_FLAGS)

bool FVaRestWsRedirectHangsAfterRedirectTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	if (!HasSilentListener(*this, Site))
	{
		return false;
	}
	// Task 11 review I-1 (plan decision 10): a redirect LWS follows to a target that hangs ends Redirect, whichever timer cuts
	// it -- here VaRest's ConnectTimeout, before LWS's own 20 s stage. The target is the server's silent listener on loopback:
	// it takes the TCP connect and never answers the TLS ClientHello, so the redirect's handshake hangs after the WSI_CREATE
	// that marks it. The plain port would not hang: a ClientHello holds a 0x0a byte (the supported_groups extension), so its
	// readline answers 400 at once. The control: the same hang, not redirected, is VaRest's ConnectTimeout
	struct FLeg
	{
		const TCHAR* What;
		FString Url;
		bool bRedirected;
	};
	const TArray<FScheme> Both = Schemes(Site);
	const FString SilentTarget = FString::Printf(TEXT("wss://127.0.0.1:%d/"), Site.SilentPort);
	TArray<FLeg> Legs;
	TArray<FString> StatusKeys;
	for (const FScheme& Scheme : Both)
	{
		const FString Key = NewKey();
		StatusKeys.Add(Key);
		Legs.Add({ Scheme.Name, Scheme.Base + FString::Printf(TEXT("/ws/status/302?location=%s&key=%s"), *VaRest::Http::PercentEncode(SilentTarget), *Key), true });
	}
	Legs.Add({ TEXT("not redirected (control)"), SilentTarget, false });
	for (int32 Index = 0; Index < Legs.Num(); ++Index)
	{
		const FLeg& Leg = Legs[Index];
		const int32 AcceptedBefore = FetchJson(*this, Site, TEXT("/tcp/silent"))[TEXT("accepted")].AsInt(-1);
		const VaRest::WebSocket::Testing::FLwsCounters Before = Transport->GetCounters();
		FVaRestSocketConnectRequest Req;
		Req.Url = Leg.Url;
		Req.ConnectTimeout = 1.0;
		const double Started = FPlatformTime::Seconds();
		FEvents Events(Transport->Connect(MoveTemp(Req)));
		TestTrue(FString::Printf(TEXT("%s: ended within 5 s"), Leg.What), Events.WaitEnd(5.0));
		const FVaRestSocketEvent* End = Events.GetEnd();
		if (!TestNotNull(FString::Printf(TEXT("%s: a terminal event"), Leg.What), End))
		{
			Events.Connection->Close(1000, TEXT("done"));
			Events.WaitEnd(30.0);
			continue;
		}
		const double Took = End->Time - Started;
		const bool bPrivateRefusal = Leg.bRedirected && VaRest::WebSocket::Testing::UsesPrivateNumericRoute();
		if (bPrivateRefusal)
			TestTrue(FString::Printf(TEXT("%s: refused before the deadline, %.3f s after connect"), Leg.What, Took), Took >= 0.0 && Took < 0.95);
		else
			// Preserve the real silent-TLS control and legacy redirect timeout margins.
			TestTrue(FString::Printf(TEXT("%s: ended at the deadline, %.3f s after the connect (1 s asked)"), Leg.What, Took), Took >= 0.95 && Took < 1.5);
		const bool bRedirectSaid = End->End.Detail.Contains(TEXT("which VaRest does not follow"), ESearchCase::CaseSensitive);
		const bool bDeadlineSaid = End->End.Detail.Contains(TEXT("VaRest's ConnectTimeout (1 s) passed"), ESearchCase::CaseSensitive);
		if (Leg.bRedirected)
		{
			if (bPrivateRefusal)
				TestTrue(FString::Printf(TEXT("%s: Error Redirect 302, By Transport, without deadline (%s)"), Leg.What, *Events.Describe()),
					End->Type == EVaRestSocketEventType::Error && End->End.Failure == EVaRestSocketFailure::Redirect && End->End.HttpStatus == 302
					&& End->End.By == EVaRestSocketCloseBy::Transport && !bDeadlineSaid);
			else
				TestTrue(FString::Printf(TEXT("%s: Error Redirect, status 0, By Transport, the Detail naming the redirect and the deadline (%s)"), Leg.What, *Events.Describe()),
					End->Type == EVaRestSocketEventType::Error && End->End.Failure == EVaRestSocketFailure::Redirect && End->End.HttpStatus == 0
					&& End->End.By == EVaRestSocketCloseBy::Transport && bRedirectSaid && bDeadlineSaid);
			TestEqual(FString::Printf(TEXT("%s: the 302 was asked once"), Leg.What), FetchJournal(*this, Site, StatusKeys[Index])[TEXT("connections")].AsInt(), 1);
		}
		else
		{
			TestTrue(FString::Printf(TEXT("%s: Error ConnectTimeout, By Timeout, VaRest's own (%s)"), Leg.What, *Events.Describe()),
				End->Type == EVaRestSocketEventType::Error && End->End.Failure == EVaRestSocketFailure::ConnectTimeout
				&& End->End.By == EVaRestSocketCloseBy::Timeout && bDeadlineSaid && !bRedirectSaid);
		}
		TestTrue(FString::Printf(TEXT("%s: the actual attempt is destroyed"), Leg.What),
			VaRest::Http::Testing::PumpUntil([&Transport] { return Transport->GetCounters().LiveConnections == 0; }, 5.0, true));
		const VaRest::WebSocket::Testing::FLwsCounters After = Transport->GetCounters();
		const int32 ExpectedHops = bPrivateRefusal ? 0 : 1;
		TestEqual(FString::Printf(TEXT("%s: actual kill count"), Leg.What), After.KillsIssued - Before.KillsIssued, ExpectedHops);
		TestEqual(FString::Printf(TEXT("%s: actual landed kills"), Leg.What), After.KillsLanded - Before.KillsLanded, ExpectedHops);
		// Real peer witness: private redirects never reach TCP; legacy and the direct control each do.
		TestEqual(FString::Printf(TEXT("%s: actual silent listener connections"), Leg.What),
			FetchJson(*this, Site, TEXT("/tcp/silent"))[TEXT("accepted")].AsInt(-1) - AcceptedBefore, ExpectedHops);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsRedirectUnresolvableKillGuardTest, "VaRest.Integration.WebSocket.Transport.Redirect.UnresolvableKillGuard", VAREST_WS_TEST_FLAGS)

bool FVaRestWsRedirectUnresolvableKillGuardTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	// Task 11 review M-4: Redirect.Unresolvable with the reap left to Kill's own guard (bOnlyKillReapsForTest). The wsi LWS
	// freed is still held when the deadline comes; the deadline's Kill must reap it instead of arming a kill on freed memory --
	// the guard a Close drained in the lws_service call that dropped the wsi relies on. Without the pass-end reap nothing
	// marks the attempt Redirect first, so the deadline reports ConnectTimeout: this switch exists only for this row
	VaRest::WebSocket::Testing::FLwsTransportOptions Options;
	Options.bOnlyKillReapsForTest = true;
	const TSharedPtr<VaRest::WebSocket::Testing::ILwsTestTransport, ESPMode::ThreadSafe> Guarded = MakeTransport(*this, Site, true, MoveTemp(Options));
	if (!Guarded.IsValid())
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Guarded->Shutdown();
	};
	const int32 InFlightBefore = VaRest::WebSocket::Testing::GetTlsHandshakesInFlight();
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Location = FString::Printf(TEXT("%s://a..b:1/"), Scheme.bTls ? TEXT("wss") : TEXT("ws"));
		FVaRestSocketConnectRequest Req = Request(Scheme, FString::Printf(TEXT("/ws/status/302?location=%s&key=%s"), *VaRest::Http::PercentEncode(Location), *NewKey()));
		Req.ConnectTimeout = 1.0;
		FEvents Events(Guarded->Connect(MoveTemp(Req)));
		TestTrue(FString::Printf(TEXT("%s: ended within 5 s"), Scheme.Name), Events.WaitEnd(5.0));
		const FVaRestSocketEvent* End = Events.GetEnd();
		if (!TestNotNull(FString::Printf(TEXT("%s: a terminal event"), Scheme.Name), End))
		{
			Events.Connection->Close(1000, TEXT("done"));
			Events.WaitEnd(30.0);
			continue;
		}
		if (VaRest::WebSocket::Testing::UsesPrivateNumericRoute())
		{
			// This backend refuses before reset/free: this is not evidence of the legacy freed-WSI Kill branch.
			TestTrue(FString::Printf(TEXT("%s: guarded transport refuses Redirect 302 before a resolver hop (%s)"), Scheme.Name, *Events.Describe()),
				End->Type == EVaRestSocketEventType::Error && End->End.Failure == EVaRestSocketFailure::Redirect
				&& End->End.HttpStatus == 302 && End->End.By == EVaRestSocketCloseBy::Transport);
		}
		else
			TestTrue(FString::Printf(TEXT("%s: Error ConnectTimeout at the deadline (%s)"), Scheme.Name, *Events.Describe()),
				End->Type == EVaRestSocketEventType::Error && End->End.Failure == EVaRestSocketFailure::ConnectTimeout);
		TestTrue(FString::Printf(TEXT("%s: the connection left the service"), Scheme.Name),
			VaRest::Http::Testing::PumpUntil([&Guarded] { return Guarded->GetCounters().LiveConnections == 0; }, 5.0, true));
	}
	TestEqual(TEXT("Kill reaped both instead of arming a kill"), Guarded->GetCounters().KillsIssued, 0);
	TestEqual(TEXT("the wss attempt left the handshakes in flight (VaRest-rti)"), VaRest::WebSocket::Testing::GetTlsHandshakesInFlight(), InFlightBefore);
	return true;
}

#undef VAREST_WS_BEGIN
#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
