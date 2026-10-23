// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 2.2 TLS: the engine's roots and pins, IP-literal hosts (VaRest-sjj), wss+insecure, refusals named

#if WITH_DEV_AUTOMATION_TESTS

#include "Interfaces/IPluginManager.h"
#include "Interfaces/ISslCertificateManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Ssl.h"
#include "VaRestHttpTesting.h"
#include "VaRestWsTestUtil.h"

namespace VaRest::WsTest::VaRestWsTlsTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;

	/** Game thread: the runner's own TLS URL form: an IP literal (Tools/run_tests.sh, --host 127.0.0.1) */
	FScheme IpLiteral(const FSite& Site)
	{
		return { TEXT("wss-ip"), FString::Printf(TEXT("wss://%s:%d"), *Site.Host, Site.TlsPort), true };
	}

	/** Game thread: Visit with an asserted terminal event, draining a timeout through Shutdown. */
	TUniquePtr<FEvents> VisitChecked(FAutomationTestBase& Test, ILwsTestTransport& Transport, FVaRestSocketConnectRequest&& Req)
	{
		TUniquePtr<FEvents> Events = Visit(Transport, MoveTemp(Req));
		if (!Test.TestNotNull(TEXT("the TLS attempt drained"), Events->GetEnd()))
		{
			Transport.Shutdown();
			Events->Pull();
		}
		return Events;
	}

	/** Any thread: reads the collected terminal event. */
	const FVaRestSocketEvent* TlsRefusal(const FEvents& Events)
	{
		const FVaRestSocketEvent* End = Events.GetEnd();
		return End && End->Type == EVaRestSocketEventType::Error && End->End.Failure == EVaRestSocketFailure::TlsVerification ? End : nullptr;
	}
}

#define VAREST_WS_TEST_FLAGS (EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTlsIpLiteralTest, "VaRest.Integration.WebSocket.Transport.Tls.IpLiteral", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTlsIpLiteralTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsTlsTestPrivate;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site);
	if (!Transport.IsValid())
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Transport->Shutdown();
	};
	// VaRest-sjj: the test certificate has IP:127.0.0.1 in its SAN (Tools/testserver/certs/README.md); LWS 3.0 alone refuses it
	// with X509 error 62 [tls.hostname_ip_literal], VaRest's own check lets it through (plan decision 9)
	const FScheme Ip = IpLiteral(Site);
	FEvents Events(Transport->Connect(Request(Ip, TEXT("/ws/echo?key=") + NewKey())));
	const bool bOpened = Events.WaitType(EVaRestSocketEventType::Open);
	if (TestTrue(FString::Printf(TEXT("wss to an IP literal with an IP SAN opens (%s)"), *Events.Describe()), bOpened))
	{
		Events.Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("over the IP"))));
		TestTrue(TEXT("... and echoes"), Events.WaitMessages(1));
		Events.Connection->Close(1000, TEXT("done"));
	}
	if (!TestTrue(TEXT("the IP connection drains"), Events.WaitEnd()))
	{
		Transport->Shutdown();
		Events.Pull();
		return false;
	}
	FEvents Server(Transport->Connect(Request(Ip, TEXT("/ws/close/4001?wait_for_message=1&key=") + NewKey())));
	if (TestTrue(TEXT("the peer-close leg observes Open before releasing the server"), Server.WaitType(EVaRestSocketEventType::Open)))
	{
		TestTrue(TEXT("release the peer close after Open"), Server.Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("release")))));
	}
	if (!TestTrue(TEXT("the peer-close leg ends without a client Close"), Server.WaitEnd()))
	{
		Transport->Shutdown();
		Server.Pull();
	}
	const FVaRestSocketEvent* End = Server.GetEnd();
	TestTrue(FString::Printf(TEXT("... and a server close arrives over it (%s)"), *Server.Describe()),
		End && End->Type == EVaRestSocketEventType::Closed && End->End.Code == 4001 && End->End.By == EVaRestSocketCloseBy::Server);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTlsRefusedTest, "VaRest.Integration.WebSocket.Transport.Tls.Refused", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTlsRefusedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsTlsTestPrivate;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	// The test CA is not trusted here: only the engine's roots
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Untrusting = MakeTransport(*this, Site, false);
	if (!Untrusting.IsValid())
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Untrusting->Shutdown();
	};
	const FString NameKey = NewKey();
	const TUniquePtr<FEvents> ByName = VisitChecked(*this, *Untrusting, Request(Schemes(Site)[1], TEXT("/ws/echo?key=") + NameKey));
	const FVaRestSocketEvent* NameEnd = TlsRefusal(*ByName);
	TestTrue(FString::Printf(TEXT("an untrusted chain is TlsVerification, naming X509 error 19 [tls.untrusted_rejected] (%s)"), *ByName->Describe()),
		NameEnd && NameEnd->End.Detail.Contains(TEXT("X509 error 19"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("... and no request reached the server"), FetchJournal(*this, Site, NameKey, 1, 300)[TEXT("connections")].AsInt(), 0);
	// The IP check clears a host-name mismatch only, at the leaf: a chain error comes first and stands (plan decision 9)
	const TUniquePtr<FEvents> ByIp = VisitChecked(*this, *Untrusting, Request(IpLiteral(Site), TEXT("/ws/echo?key=") + NewKey()));
	const FVaRestSocketEvent* IpEnd = TlsRefusal(*ByIp);
	TestTrue(FString::Printf(TEXT("an untrusted chain to an IP with an IP SAN is still refused, error 19 (%s)"), *ByIp->Describe()),
		IpEnd && IpEnd->End.Detail.Contains(TEXT("X509 error 19"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTlsWrongIpTest, "VaRest.Integration.WebSocket.Transport.Tls.WrongIp", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTlsWrongIpTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsTlsTestPrivate;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	// Ruling Q3, through the verify callback (review conc M-10): wss://10.0.0.1 with TCP to the server (127.0.0.1), whose leaf
	// has IP:127.0.0.1 and IP:::1 only. The chain is trusted (the test CA); the IP is not the host's, so error 62 stands
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site);
	if (!Transport.IsValid())
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Transport->Shutdown();
	};
	const FScheme Wrong = { TEXT("wss-wrong-ip"), FString::Printf(TEXT("wss://10.0.0.1:%d"), Site.TlsPort), true };
	const FString Key = NewKey();
	const TUniquePtr<FEvents> Events = VisitChecked(*this, *Transport, Request(Wrong, TEXT("/ws/echo?key=") + Key));
	const FVaRestSocketEvent* End = TlsRefusal(*Events);
	TestTrue(FString::Printf(TEXT("a certificate for another IP is TlsVerification, X509 error 62 at depth 0 (%s)"), *Events->Describe()),
		End && End->End.Detail.Contains(TEXT("X509 error 62 at depth 0"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("... and no request reached the server"), FetchJournal(*this, Site, Key, 1, 300)[TEXT("connections")].AsInt(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTlsInsecureTest, "VaRest.Integration.WebSocket.Transport.Tls.Insecure", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTlsInsecureTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsTlsTestPrivate;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	// Spec 2.2, 15: outside Shipping, wss+insecure and bAllowSelfSignedTls allow the self-signed chain and skip the name check
	// [tls.insecure_skip_hostname]; ALLOW_SELFSIGNED alone would fail the IP host [tls.insecure_ip_host]
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Untrusting = MakeTransport(*this, Site, false);
	if (!Untrusting.IsValid())
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Untrusting->Shutdown();
	};
	FVaRestSocketConnectRequest Insecure;
	Insecure.Url = FString::Printf(TEXT("wss+insecure://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *NewKey());
	const TUniquePtr<FEvents> Scheme = VisitChecked(*this, *Untrusting, MoveTemp(Insecure));
	TestNotNull(FString::Printf(TEXT("wss+insecure to an IP with an untrusted chain opens (%s)"), *Scheme->Describe()), Scheme->Find(EVaRestSocketEventType::Open));
	FVaRestSocketConnectRequest Allowed = Request(IpLiteral(Site), TEXT("/ws/echo?key=") + NewKey());
	Allowed.bAllowSelfSignedTls = true;
	const TUniquePtr<FEvents> Flag = VisitChecked(*this, *Untrusting, MoveTemp(Allowed));
	TestNotNull(FString::Printf(TEXT("bAllowSelfSignedTls on wss does the same (%s)"), *Flag->Describe()), Flag->Find(EVaRestSocketEventType::Open));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTlsPinTest, "VaRest.Integration.WebSocket.Transport.Tls.PinMismatch", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTlsPinTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsTlsTestPrivate;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	ISslCertificateManager& Certificates = FSslModule::Get().GetCertificateManager();
	// The host project pins nothing; with pins of its own this test could not clear them back (ClearAllPinnedPublicKeys)
	if (!TestFalse(TEXT("no pinned keys before the test"), Certificates.HasPinnedPublicKeys()))
	{
		return false;
	}
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site);
	if (!Transport.IsValid())
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Transport->Shutdown();
		Certificates.ClearAllPinnedPublicKeys();
	};
	// Spec 2.2: keys pinned on the engine's certificate manager apply for free (ISslCertificateManager.h:38-56); a SHA-256 of
	// 32 zero bytes matches no key
	Certificates.SetPinnedPublicKeys(TEXT("localhost"), TEXT("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="));
	const TUniquePtr<FEvents> Pinned = VisitChecked(*this, *Transport, Request(Schemes(Site)[1], TEXT("/ws/echo?key=") + NewKey()));
	const FVaRestSocketEvent* End = TlsRefusal(*Pinned);
	TestTrue(FString::Printf(TEXT("a pin that does not match is TlsVerification (%s)"), *Pinned->Describe()), End && End->End.Detail.Contains(TEXT("pinned"), ESearchCase::CaseSensitive));
	FVaRestSocketConnectRequest InsecurePinned = Request(Schemes(Site)[1], TEXT("/ws/echo?key=") + NewKey());
	InsecurePinned.bAllowSelfSignedTls = true;
	const TUniquePtr<FEvents> Insecure = VisitChecked(*this, *Transport, MoveTemp(InsecurePinned));
	TestNotNull(FString::Printf(TEXT("insecure mode skips the same engine pin (%s)"), *Insecure->Describe()), Insecure->Find(EVaRestSocketEventType::Open));
	// The manager has no lock around pin mutation: wait for the service to destroy the attempt before changing pins.
	if (!TestTrue(TEXT("the pinned attempts are destroyed"), VaRest::Http::Testing::PumpUntil([Transport] { return Transport->GetCounters().LiveConnections == 0; }, 5.0, true)))
	{
		return false;
	}
	Certificates.ClearAllPinnedPublicKeys();
	const TUniquePtr<FEvents> Unpinned = VisitChecked(*this, *Transport, Request(Schemes(Site)[1], TEXT("/ws/echo?key=") + NewKey()));
	TestNotNull(FString::Printf(TEXT("without it the same server opens (%s)"), *Unpinned->Describe()), Unpinned->Find(EVaRestSocketEventType::Open));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTlsCertificateIpTest, "VaRest.Integration.WebSocket.Transport.Tls.CertificateIp", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTlsCertificateIpTest::RunTest(const FString& Parameters)
{
	VaRest::WsTest::ExpectLwsLines(*this);
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VaRest"));
	if (!TestTrue(TEXT("the VaRest plugin"), Plugin.IsValid()))
	{
		return false;
	}
	TArray<uint8> Pem;
	const FString Path = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Tools/testserver/certs/test-server.pem"));
	if (!TestTrue(FString::Printf(TEXT("the test server's certificate (%s)"), *Path), FFileHelper::LoadFileToArray(Pem, *Path)))
	{
		return false;
	}
	// The leaf's SAN: DNS:localhost, IP:127.0.0.1, IP:::1 (Tools/testserver/certs/README.md)
	TestTrue(TEXT("127.0.0.1 is in the SAN"), VaRest::WebSocket::Testing::CertificateMatchesIp(Pem, TEXT("127.0.0.1")));
	TestTrue(TEXT("::1 is"), VaRest::WebSocket::Testing::CertificateMatchesIp(Pem, TEXT("::1")));
	TestFalse(TEXT("10.0.0.1 is not"), VaRest::WebSocket::Testing::CertificateMatchesIp(Pem, TEXT("10.0.0.1")));
	TestFalse(TEXT("a name is not an IP"), VaRest::WebSocket::Testing::CertificateMatchesIp(Pem, TEXT("localhost")));
	TestFalse(TEXT("no certificate matches nothing"), VaRest::WebSocket::Testing::CertificateMatchesIp(TConstArrayView<uint8>(), TEXT("127.0.0.1")));
	return true;
}

#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
