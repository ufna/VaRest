// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// Ruling Q2 (VaRest-2rt): an upgrade redirect is never followed -- to the same port, an absolute URL, wss, or a downgrade

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestUrl.h"
#include "VaRestWsTestUtil.h"

namespace VaRest::WsTest::VaRestWsRedirectTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;

	/** /ws/status/302 to Location, with an Authorization header; checks the attempt failed Redirect and the target saw nothing */
	void ExpectRefused(FAutomationTestBase& Test, ILwsTestTransport& Transport, const FSite& Site, const FScheme& Scheme, const FString& Location, const FString& TargetKey, const TCHAR* What)
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, FString::Printf(TEXT("/ws/status/302?location=%s&key=%s"), *VaRest::Http::PercentEncode(Location), *Key));
		Req.Headers = { { TEXT("Authorization"), TEXT("Bearer redirect-test-token") } };
		const TUniquePtr<FEvents> Events = Visit(Transport, MoveTemp(Req));
		const FVaRestSocketEvent* End = Events->GetEnd();
		Test.TestTrue(FString::Printf(TEXT("%s %s: Error Redirect, status 0 (plan decision 10) (%s)"), Scheme.Name, What, *Events->Describe()),
			End && End->Type == EVaRestSocketEventType::Error && End->End.Failure == EVaRestSocketFailure::Redirect && End->End.HttpStatus == 0);
		Test.TestEqual(FString::Printf(TEXT("%s %s: the 302 was asked once"), Scheme.Name, What), FetchJournal(Test, Site, Key)[TEXT("connections")].AsInt(), 1);
		// The target never got a request, so the Authorization header never reached it (ruling Q2)
		Test.TestEqual(FString::Printf(TEXT("%s %s: the target saw no request"), Scheme.Name, What), FetchJournal(Test, Site, TargetKey, 1, 1000)[TEXT("connections")].AsInt(), 0);
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
		ExpectRefused(*this, *Transport, Site, Scheme, TEXT("/ws/echo?key=") + Relative, Relative, TEXT("relative"));
		// hs.redirect_absolute: the same server by absolute URL. By IP: LWS resolves a Location's host itself, and localhost is ::1
		// first here (plan decision 19), so a name would fail the TCP connect before the redirect could be seen
		const FString Absolute = NewKey();
		ExpectRefused(*this, *Transport, Site, Scheme, FString::Printf(TEXT("%s://%s:%d/ws/echo?key=%s"), Scheme.bTls ? TEXT("wss") : TEXT("ws"), *Site.Host,
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
	ExpectRefused(*this, *Transport, Site, Both[0], FString::Printf(TEXT("wss://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *ToTls), ToTls, TEXT("to wss"));
	// hs.redirect_downgrade: wss -> ws, which LWS refuses itself ("HS: Redirect attempted SSL downgrade")
	const FString Down = NewKey();
	ExpectRefused(*this, *Transport, Site, Both[1], FString::Printf(TEXT("ws://%s:%d/ws/echo?key=%s"), *Site.Host, Site.Port, *Down), Down, TEXT("downgrade"));
	return true;
}

#undef VAREST_WS_BEGIN
#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
