// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// Ruling Q2 (VaRest-2rt): an upgrade redirect is never followed -- to the same port, an absolute URL, wss, a downgrade, with
// any redirect status, or to a target that refuses TCP

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
	 * /ws/status/<Status> to Location, with an Authorization header; checks the attempt failed Redirect and, when TargetKey is
	 * given, that the target saw nothing
	 */
	void ExpectRefused(FAutomationTestBase& Test, ILwsTestTransport& Transport, const FSite& Site, const FScheme& Scheme, int32 Status, const FString& Location,
		const FString& TargetKey, const TCHAR* What)
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, FString::Printf(TEXT("/ws/status/%d?location=%s&key=%s"), Status, *VaRest::Http::PercentEncode(Location), *Key));
		Req.Headers = { { TEXT("Authorization"), TEXT("Bearer redirect-test-token") } };
		const TUniquePtr<FEvents> Events = Visit(Transport, MoveTemp(Req));
		const FVaRestSocketEvent* End = Events->GetEnd();
		Test.TestTrue(FString::Printf(TEXT("%s %s: Error Redirect, status 0 (plan decision 10) (%s)"), Scheme.Name, What, *Events->Describe()),
			End && End->Type == EVaRestSocketEventType::Error && End->End.Failure == EVaRestSocketFailure::Redirect && End->End.HttpStatus == 0);
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
		// hs.redirect_relative: LWS follows a relative Location on its own; the second APPEND_HANDSHAKE_HEADER is refused
		const FString Relative = NewKey();
		ExpectRefused(*this, *Transport, Site, Scheme, 302, TEXT("/ws/echo?key=") + Relative, Relative, TEXT("relative"));
		// hs.redirect_absolute: the same server by absolute URL. By IP: LWS resolves a Location's host itself, and localhost is ::1
		// first here (plan decision 19), so a name would fail the TCP connect before the redirect could be seen
		const FString Absolute = NewKey();
		ExpectRefused(*this, *Transport, Site, Scheme, 302, FString::Printf(TEXT("%s://%s:%d/ws/echo?key=%s"), Scheme.bTls ? TEXT("wss") : TEXT("ws"), *Site.Host,
			Scheme.bTls ? Site.TlsPort : Site.Port, *Absolute), Absolute, TEXT("absolute"));
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
	ExpectRefused(*this, *Transport, Site, Both[0], 302, FString::Printf(TEXT("wss://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *ToTls), ToTls, TEXT("to wss"));
	// hs.redirect_downgrade: wss -> ws, which LWS refuses itself ("HS: Redirect attempted SSL downgrade")
	const FString Down = NewKey();
	ExpectRefused(*this, *Transport, Site, Both[1], 302, FString::Printf(TEXT("ws://%s:%d/ws/echo?key=%s"), *Site.Host, Site.Port, *Down), Down, TEXT("downgrade"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsRedirectStatusesTest, "VaRest.Integration.WebSocket.Transport.Redirect.Statuses", VAREST_WS_TEST_FLAGS)

bool FVaRestWsRedirectStatusesTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	// Task 10 review M-2: every redirect status, not only 302. The refusal does not depend on the status (it keys on what LWS
	// does once it follows), but which statuses LWS 3.0 follows was not measured
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
		TestTrue(FString::Printf(TEXT("%s: Error Redirect, status 0, after %.2f s (%s)"), Scheme.Name, End->Time - Started, *Events.Describe()),
			End->Type == EVaRestSocketEventType::Error && End->End.Failure == EVaRestSocketFailure::Redirect && End->End.HttpStatus == 0);
		TestFalse(FString::Printf(TEXT("%s: the Detail names no host"), Scheme.Name), End->End.Detail.Contains(TEXT("a..b"), ESearchCase::IgnoreCase));
		TestEqual(FString::Printf(TEXT("%s: the 302 was asked once"), Scheme.Name), FetchJournal(*this, Site, Key)[TEXT("connections")].AsInt(), 1);
		// Gone from the service, with no kill: LWS freed the wsi, so nothing may touch it again
		TestTrue(FString::Printf(TEXT("%s: the connection left the service"), Scheme.Name),
			VaRest::Http::Testing::PumpUntil([&Transport] { return Transport->GetCounters().LiveConnections == 0; }, 5.0, true));
	}
	TestEqual(TEXT("no kill was issued"), Transport->GetCounters().KillsIssued, 0);
	TestEqual(TEXT("the wss attempt left the handshakes in flight (VaRest-rti)"), VaRest::WebSocket::Testing::GetTlsHandshakesInFlight(), InFlightBefore);
	return true;
}

#undef VAREST_WS_BEGIN
#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
