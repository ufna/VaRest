// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.4 (W0): TLS through the engine's glue (the SSL module's context or LWS's own, the certificate
// manager's roots and verify step), host-name checks against the test certificate, a provided SSL_CTX (the monolithic
// path's mechanics), and an engine IWebSocket next to a probe context, ws and wss (WebSocket spec 1.2, 2.1, 2.2)

#include "VaRestLwsProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "IWebSocket.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/StringOutputDevice.h"
#include "Modules/ModuleManager.h"
#include "Ssl.h"
#include "VaRestHttpTesting.h"
#include "WebSocketsModule.h"

#if PLATFORM_LINUX
#include <dlfcn.h>

// libwebsockets.a's ssl.c.o defines this as a common symbol (nm: C) and declares it in no header; lws_context_init_ssl_library
// stores a fresh CRYPTO_get_ex_new_index into it on every lws_create_context (WS W0 decision 16, review I-3). Weak (W0 final
// review M-2): it is LWS-private, and a bundled LWS that renames it or makes it static must not break this module's link,
// which also holds the default suite. Absent, its address is null and the fact reads unverified. ssl.c.o is still linked:
// lws_create_context reaches lws_context_init_ssl_library, which lives there, so the weak reference binds to it
extern "C" __attribute__((weak)) int openssl_websocket_private_data_index;
#endif

namespace VaRest::LwsProbe::VaRestLwsProbeTlsTestPrivate
{
	/** The domain the engine verifies against: the Host header (or address) without its port */
	FString DomainOf(const FConn& Conn)
	{
		const FString Host = !Conn.Spec.Host.IsEmpty() ? Conn.Spec.Host : !Conn.Spec.Address.IsEmpty() ? Conn.Spec.Address : Conn.Rig->GetServer().Host;
		int32 Colon = INDEX_NONE;
		return Host.FindChar(TEXT(':'), Colon) ? Host.Left(Colon) : Host;
	}

	/**
	 * The engine's verify step (LwsWebSocket.cpp:549-572, bDisableCertValidation off): a chain OpenSSL accepted still
	 * passes the certificate manager's check (pinned keys) for the URL's domain; anything else fails
	 */
	int32 EngineVerify(FConn& Conn, X509_STORE_CTX* Store, int64 PreverifyOk)
	{
		int32 Ok = static_cast<int32>(PreverifyOk);
		const FString Domain = DomainOf(Conn);
		if (Ok == 1 && !FSslModule::Get().GetCertificateManager().VerifySslCertificates(Store, Domain))
		{
			Ok = 0;
		}
		Conn.Rig->Record(Conn.Id, Marker, Ok, FString::Printf(TEXT("engine verify domain=%s ok=%d"), *Domain, Ok));
		return Ok == 1 ? 0 : 1;
	}

	/** The engine's roots, as LwsWebSocketsManager.cpp:441-446 adds them */
	void EngineRoots(SSL_CTX* Ctx)
	{
		FSslModule::Get().GetCertificateManager().AddCertificatesToSslContext(Ctx);
	}

	/** On ESTABLISHED: the negotiated TLS version, as a Marker */
	int32 RecordTlsVersion(FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len)
	{
		if (Reason == LWS_CALLBACK_CLIENT_ESTABLISHED)
		{
			SSL* Ssl = lws_get_ssl(Wsi);
			const char* Version = Ssl ? SSL_get_version(Ssl) : "none";
			C.Rig->Record(C.Id, Marker, 0, FString::Printf(TEXT("tls=%s"), *Utf8Text(Version, FCStringAnsi::Strlen(Version))));
		}
		return KeepDefault;
	}

	/** A wss connection to the TLS listener: TCP to 127.0.0.1, Host (and the checked name) as given */
	FConnectSpec Tls(const FServer& Site, const FString& HostName, int32 Flags)
	{
		FConnectSpec Spec;
		Spec.bTls = true;
		Spec.Address = Site.Host;
		Spec.Host = FString::Printf(TEXT("%s:%d"), *HostName, Site.TlsPort);
		Spec.SslFlags = Flags;
		Spec.Path = TEXT("/ws/echo?key=") + NewKey();
		Spec.Hook = &RecordTlsVersion;
		return Spec;
	}

	/** "established (tls=...)" or "refused: ...", with the verify callback's notes */
	FString TlsOutcome(const TArray<FEvent>& Seen)
	{
		TArray<FString> Notes;
		for (const FEvent& Each : Seen)
		{
			if (Each.Reason == LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION || Each.Reason == Marker)
			{
				Notes.Add(Each.Note);
			}
		}
		return FString::Printf(TEXT("%s [%s]"), Opened(Seen) ? TEXT("established") : *FString::Printf(TEXT("refused: %s"), *ErrorOf(Seen)),
			*FString::Join(Notes, TEXT("; ")));
	}

	bool LoadExtraFired(FRig& Rig)
	{
		return Rig.Events().ContainsByPredicate([](const FEvent& Each) { return Each.Reason == LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS; });
	}

	/** One engine IWebSocket and what it reported (delegates run on the game thread, from the core ticker) */
	struct FEngineSide
	{
		bool bConnected = false;
		bool bEnded = false;
		FString Error;
		TArray<FString> Messages;
	};

	TSharedRef<IWebSocket> OpenEngineSocket(FWebSocketsModule& Module, const FString& Url, const TSharedRef<FEngineSide>& Side)
	{
		// CreateWebSocket is virtual (WebSocketsModule.h:72): no link to the WebSockets module (decision 12). Connect from the
		// test body, never from a handler (WebSocket spec E25)
		TSharedRef<IWebSocket> Socket = Module.CreateWebSocket(Url, FString());
		Socket->OnConnected().AddLambda([Side] { Side->bConnected = true; });
		Socket->OnMessage().AddLambda([Side](const FString& Message) { Side->Messages.Add(Message); });
		Socket->OnConnectionError().AddLambda([Side](const FString& Reason) { Side->Error = Reason; Side->bEnded = true; });
		Socket->OnClosed().AddLambda([Side](int32 Code, const FString& Reason, bool bWasClean) { Side->bEnded = true; });
		Socket->Connect();
		return Socket;
	}

	bool EngineEchoed(const TSharedRef<FEngineSide>& Side, const FString& Text)
	{
		return Side->Messages.ContainsByPredicate([&Text](const FString& Each) { return Each.Equals(Text, ESearchCase::CaseSensitive); });
	}

	/** Both open at once, both echo, both close and end; false when any step fails */
	bool BothEcho(FRig& Rig, int32 Conn, const TSharedRef<IWebSocket>& Socket, const TSharedRef<FEngineSide>& Side, FString& OutWhy)
	{
		Rig.WaitFor([Conn, Side](const TArray<FEvent>& All)
		{
			const bool bProbeSettled = All.ContainsByPredicate([Conn](const FEvent& Each)
			{
				return Each.Conn == Conn && (Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED || Each.Reason == LWS_CALLBACK_WSI_DESTROY);
			});
			return bProbeSettled && (Side->bConnected || Side->bEnded);
		}, 20.0);
		const bool bBothOpen = Side->bConnected && Opened(Rig.Events(Conn));
		bool bEchoed = false;
		if (bBothOpen)
		{
			Socket->Send(TEXT("engine-hello"));
			Rig.WriteText(Conn, TEXT("probe-hello"));
			bEchoed = Rig.WaitFor([Conn, Side](const TArray<FEvent>& All)
			{
				return EngineEchoed(Side, TEXT("engine-hello")) && All.ContainsByPredicate([Conn](const FEvent& Each)
				{
					return Each.Conn == Conn && Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE && EventText(Each).Equals(TEXT("probe-hello"), ESearchCase::CaseSensitive);
				});
			});
		}
		// Both drain, whatever happened
		if (Side->bConnected && !Side->bEnded)
		{
			Socket->Close(1000, TEXT("done"));
		}
		Rig.Close(Conn, 1000, TEXT("done"));
		const bool bEnded = Rig.WaitFor([Conn, Side](const TArray<FEvent>& All)
		{
			return (Side->bEnded || !Side->bConnected) && All.ContainsByPredicate([Conn](const FEvent& Each)
			{
				return Each.Conn == Conn && (Each.Reason == LWS_CALLBACK_WSI_DESTROY || (Each.Reason == ConnectReturned && Each.Note == TEXT("null")));
			});
		}, 20.0);
		OutWhy = FString::Printf(TEXT("engine: connected=%s ended=%s error=\"%s\" echoed=%s; probe: opened=%s; both ended=%s"),
			YesNo(Side->bConnected), YesNo(Side->bEnded), *Side->Error, YesNo(EngineEchoed(Side, TEXT("engine-hello"))), YesNo(Opened(Rig.Events(Conn))), YesNo(bEnded));
		return bBothOpen && bEchoed && bEnded;
	}

	/**
	 * A log category's current verbosity as "Log list <cat>" prints it ("%-40s  %-12s", LogSuppressionInterface.cpp:593-614),
	 * or empty when it is not listed or exec commands are compiled out
	 */
	FString LogVerbosityOf(const TCHAR* Category)
	{
		FString Out;
#if UE_ALLOW_EXEC_COMMANDS
		if (!GEngine)
		{
			// The caller warns: an empty result means the verbosity is neither raised nor restored
			return Out;
		}
		FStringOutputDevice Listing;
		Listing.SetAutoEmitLineTerminator(true);
		GEngine->Exec(nullptr, *FString::Printf(TEXT("Log list %s"), Category), Listing);
		TArray<FString> Lines;
		Listing.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			TArray<FString> Tokens;
			Line.ParseIntoArrayWS(Tokens);
			// "Log list" matches substrings: take only the category itself
			if (Tokens.Num() >= 2 && Tokens[0].Equals(Category, ESearchCase::CaseSensitive))
			{
				Out = Tokens[1];
			}
		}
#endif
		return Out;
	}

	/** lwsl_visible reads this module's copy of LWS's log level (libwebsockets.h:434-443) */
	FString LogBits()
	{
		return FString::Printf(TEXT("err=%d warn=%d notice=%d info=%d client=%d"), lwsl_visible(LLL_ERR), lwsl_visible(LLL_WARN), lwsl_visible(LLL_NOTICE),
			lwsl_visible(LLL_INFO), lwsl_visible(LLL_CLIENT));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeTlsEnginePathTest, "Probe.VaRest.Lws.Tls.EnginePath",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeTlsEnginePathTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeTlsTestPrivate;
	// The engine's ::FEvent (HAL/Event.h) would make the name ambiguous under the using-directive
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site, true))
	{
		return false;
	}
	// OpenSSL's verify errors may reach the engine's LWS log routing when the copy is shared (coexist.shared_lws_state)
	AddExpectedMessagePlain(TEXT("Lws("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	FRigOptions Options;
	Options.Tls = ETls::SslModule;
	Options.OnLoadExtraClientVerifyCerts = &EngineRoots;
	Options.OnVerifyServerCert = &EngineVerify;
	FRig Rig(Site, MoveTemp(Options));
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	// Observed is what the rig measured (whether a context came from the SSL module and was provided); why it did not is
	// the source's, so it goes in the evidence (review minor 2)
	Fact(*this, TEXT("tls.ssl_module_ctx"), AgreesIf(Rig.UsedSslModuleContext()),
		Rig.UsedSslModuleContext() ? FString(TEXT("the SSL module's SSL_CTX (ISslManager::CreateSslContext) was provided"))
			: FString::Printf(TEXT("the SSL module's SSL_CTX was not used in this %s build: no context came from ISslManager, so the rig fell back to DO_SSL_GLOBAL_INIT"), IS_MONOLITHIC ? TEXT("monolithic") : TEXT("modular")),
		TEXT("WebSocket spec 2.1: SSL_CTX from ISslManager::CreateSslContext (ISslManager.h:64-81)"),
		TEXT("FRigOptions::Tls = SslModule: the engine's order, LwsWebSocketsManager.cpp:205-225; InitializeSsl() succeeds only when IS_MONOLITHIC || UE_MERGED_MODULES (SslManager.cpp:26-55) and CreateSslContext returns null otherwise (:76-131)"));
	Fact(*this, TEXT("tls.load_extra_fired"), AgreesIf(LoadExtraFired(Rig)), YesNo(LoadExtraFired(Rig)),
		TEXT("WebSocket spec 2.2: LOAD_EXTRA_CLIENT_VERIFY_CERTS -> AddCertificatesToSslContext"),
		TEXT("the context's callbacks during lws_create_context"));
	TArray<FEvent> LocalSeen;
	TArray<FEvent> IpSeen;
	TArray<FEvent> SkipSeen;
	// A refused handshake, or a connection that never ends, is a fact (Visit notes it), not a failure (review M-2)
	Visit(*this, Rig, Tls(Site, TEXT("localhost"), LCCSCF_ALLOW_SELFSIGNED), LocalSeen);
	Visit(*this, Rig, Tls(Site, Site.Host, LCCSCF_ALLOW_SELFSIGNED), IpSeen);
	Visit(*this, Rig, Tls(Site, Site.Host, LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK), SkipSeen);
	const FString InsecureSpec = TEXT("WebSocket spec 2.2: wss+insecure means self-signed allowed (LCCSCF_ALLOW_SELFSIGNED, as LwsWebSocket.cpp:747-750); 16.3: the test server is reached that way");
	Fact(*this, TEXT("tls.insecure_localhost"), EVerdict::Recorded, TlsOutcome(LocalSeen), InsecureSpec,
		TEXT("LCCSCF_ALLOW_SELFSIGNED, Host localhost:<port>, the chain leaf + test CA, the engine's roots and verify step"));
	Fact(*this, TEXT("tls.insecure_ip_host"), EVerdict::Recorded, TlsOutcome(IpSeen), InsecureSpec,
		TEXT("LCCSCF_ALLOW_SELFSIGNED, Host 127.0.0.1:<port>: the engine's wss+insecure on an IP URL"));
	Fact(*this, TEXT("tls.insecure_skip_hostname"), EVerdict::Recorded, TlsOutcome(SkipSeen), InsecureSpec,
		TEXT("LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK, Host 127.0.0.1:<port>"));
	FString Version = TEXT("no TLS connection opened");
	for (const TArray<FEvent>* Seen : { &LocalSeen, &IpSeen, &SkipSeen })
	{
		const FEvent* Mark = Seen->FindByPredicate([](const FEvent& Each) { return Each.Reason == Marker && Each.Note.StartsWith(TEXT("tls=")); });
		if (Mark)
		{
			Version = Mark->Note;
			break;
		}
	}
	Fact(*this, TEXT("tls.version"), EVerdict::Recorded, Version, TEXT("none (ESslTlsProtocol stops at TLSv1_2, ISslManager.h:14-33; OpenSSL 1.1.1 also speaks 1.3)"),
		TEXT("SSL_get_version(lws_get_ssl(wsi)) at ESTABLISHED"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeTlsVerificationTest, "Probe.VaRest.Lws.Tls.Verification",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeTlsVerificationTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeTlsTestPrivate;
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site, true))
	{
		return false;
	}
	AddExpectedMessagePlain(TEXT("Lws("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	TArray<uint8> Ca;
	if (!FetchBytes(*this, Site, TEXT("/tls/ca.pem"), Ca))
	{
		return false;
	}
	// Untrusted: the engine's roots only, and full verification (LCCSCF_USE_SSL, nothing allowed)
	FRigOptions Untrusting;
	Untrusting.Tls = ETls::SslModule;
	Untrusting.OnLoadExtraClientVerifyCerts = &EngineRoots;
	Untrusting.OnVerifyServerCert = &EngineVerify;
	TArray<FEvent> UntrustedSeen;
	// One TLS rig alive at a time (decision 16)
	{
		FRig Rig(Site, MoveTemp(Untrusting));
		if (!TestTrue(TEXT("the untrusting context exists"), Rig.IsReady()))
		{
			return false;
		}
		Visit(*this, Rig, Tls(Site, TEXT("localhost"), 0), UntrustedSeen);
	}
	// Trusted: the same, with the test CA in the store
	FRigOptions Trusting;
	Trusting.Tls = ETls::SslModule;
	Trusting.OnLoadExtraClientVerifyCerts = [Ca](SSL_CTX* Ctx)
	{
		EngineRoots(Ctx);
		AddPemToStore(Ctx, Ca);
	};
	Trusting.OnVerifyServerCert = &EngineVerify;
	TArray<FEvent> TrustedSeen;
	TArray<FEvent> IpSeen;
	{
		FRig Rig(Site, MoveTemp(Trusting));
		if (!TestTrue(TEXT("the trusting context exists"), Rig.IsReady()))
		{
			return false;
		}
		Visit(*this, Rig, Tls(Site, TEXT("localhost"), 0), TrustedSeen);
		Visit(*this, Rig, Tls(Site, Site.Host, 0), IpSeen);
	}
	Fact(*this, TEXT("tls.untrusted_rejected"), AgreesIf(!Opened(UntrustedSeen)), TlsOutcome(UntrustedSeen),
		TEXT("WebSocket spec 2.2: wss verifies the chain (engine roots, VerifySslCertificates); a chain from an unknown CA is refused"),
		TEXT("LCCSCF_USE_SSL only, the test CA not trusted, Host localhost:<port>"));
	Fact(*this, TEXT("tls.trusted_ca_verified"), AgreesIf(Opened(TrustedSeen)), TlsOutcome(TrustedSeen),
		TEXT("WebSocket spec 2.2: a chain to a trusted root, then the certificate manager's check, opens"),
		TEXT("LCCSCF_USE_SSL only, the test CA added in LOAD_EXTRA_CLIENT_VERIFY_CERTS, Host localhost:<port>"));
	Fact(*this, TEXT("tls.hostname_with_port"), EVerdict::Recorded,
		Opened(TrustedSeen) ? FString(TEXT("Host \"localhost:<port>\" passed the name check: LWS checks the host without its port"))
			: FString::Printf(TEXT("the trusted connection did not open: %s"), *ErrorOf(TrustedSeen)),
		TEXT("WebSocket spec 2.2: Host keeps a non-default port (LwsWebSocket.cpp:775-785), and TLS must still check the name"),
		TEXT("the trusted connection above"));
	Fact(*this, TEXT("tls.hostname_ip_literal"), EVerdict::Recorded, TlsOutcome(IpSeen),
		TEXT("none: the certificate has IP:127.0.0.1 in its SAN; X509_VERIFY_PARAM_set1_host compares DNS names"),
		TEXT("LCCSCF_USE_SSL only, the test CA trusted, Host 127.0.0.1:<port>"));
	TArray<FString> Calls;
	for (const FEvent& Each : OfReason(TrustedSeen, LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION))
	{
		Calls.Add(Each.Note);
	}
	for (const FEvent& Each : OfReason(UntrustedSeen, LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION))
	{
		Calls.Add(TEXT("untrusted: ") + Each.Note);
	}
	Fact(*this, TEXT("tls.verify_callback_calls"), EVerdict::Recorded, Calls.Num() > 0 ? FString::Join(Calls, TEXT("; ")) : FString(TEXT("PERFORM_SERVER_CERT_VERIFICATION never called")),
		TEXT("WebSocket spec 2.2: PERFORM_SERVER_CERT_VERIFICATION -> VerifySslCertificates, with the user-data remap (LwsWebSocketsManager.cpp:447-456)"),
		TEXT("the trusted and the untrusted connection"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeTlsProvidedContextTest, "Probe.VaRest.Lws.Tls.ProvidedContext",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeTlsProvidedContextTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeTlsTestPrivate;
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site, true))
	{
		return false;
	}
	AddExpectedMessagePlain(TEXT("Lws("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	TArray<uint8> Ca;
	if (!FetchBytes(*this, Site, TEXT("/tls/ca.pem"), Ca))
	{
		return false;
	}
	// Decision 13: the mechanics of the monolithic path (a context made before lws_create_context and provided, no
	// DO_SSL_GLOBAL_INIT, LwsWebSocketsManager.cpp:210-215), with the test CA instead of the engine's roots
	SSL_CTX* Provided = SSL_CTX_new(TLS_client_method());
	if (!TestNotNull(TEXT("an SSL_CTX"), Provided))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		// After the rig: LWS does not free a provided context (libwebsockets.h:2765-2769)
		SSL_CTX_free(Provided);
	};
	TestEqual(TEXT("the test CA is in the provided context's store"), AddPemToStore(Provided, Ca), 1);
	TArray<FEvent> Seen;
	bool bLoadExtra = false;
	{
		FRigOptions Options;
		Options.Tls = ETls::ProvidedContext;
		Options.ProvidedSslContext = Provided;
		FRig Rig(Site, MoveTemp(Options));
		if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
		{
			return false;
		}
		bLoadExtra = LoadExtraFired(Rig);
		Visit(*this, Rig, Tls(Site, TEXT("localhost"), 0), Seen);
	}
	Fact(*this, TEXT("tls.provided_ctx_connects"), EVerdict::Recorded, TlsOutcome(Seen),
		TEXT("T15: TLS through a provided SSL_CTX (provided_client_ssl_ctx, libwebsockets.h:2764-2772); WebSocket spec 2.1's monolithic path"),
		TEXT("SSL_CTX_new(TLS_client_method()) + the test CA, no DO_SSL_GLOBAL_INIT, LCCSCF_USE_SSL, Host localhost:<port>"));
	Fact(*this, TEXT("tls.provided_ctx_load_extra_fired"), EVerdict::Recorded, YesNo(bLoadExtra),
		TEXT("WebSocket spec 2.2 relies on LOAD_EXTRA_CLIENT_VERIFY_CERTS for the engine's roots; with a provided context they come from CreateSslContext itself (bAddCertificates, ISslManager.h:46-47, SslManager.cpp:114-117)"),
		TEXT("the provided-context rig's callbacks during lws_create_context"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeCoexistTest, "Probe.VaRest.Lws.Coexist.EngineWebSocket",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeCoexistTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeTlsTestPrivate;
#if WITH_WEBSOCKETS
	FServer Site;
	if (!FindServer(*this, Site, true))
	{
		return false;
	}
	// The engine's LWS lines (LwsWebSocketsManager.cpp:403-424) are this test's evidence, not failures
	AddExpectedMessagePlain(TEXT("Lws("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	VaRest::Http::Testing::FLogCapture Capture;

	// 1. Process-global LWS state: one copy or two (WebSocket spec 1.2, T16)
	const bool bLoadedBefore = FModuleManager::Get().IsModuleLoaded(TEXT("WebSockets"));
	const FString Before = LogBits();
	FWebSocketsModule& Module = FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	const FString After = LogBits();
	// The engine's InitWebSockets enables LLL_CLIENT (LwsWebSocketsManager.cpp:176); LWS's default is err|warn|notice
	// (libwebsockets.h:407-418). Seen here, the engine's call reached this module's copy.
	const bool bShared = lwsl_visible(LLL_CLIENT) != 0;
	Fact(*this, TEXT("coexist.shared_lws_state"), AgreesIf(bShared),
		FString::Printf(TEXT("%s: this module's log state before loading WebSockets %s, after %s (WebSockets loaded before this test: %s)"),
			bShared ? TEXT("shared with the engine's copy") : TEXT("separate from the engine's copy"), *Before, *After, YesNo(bLoadedBefore)),
		TEXT("WebSocket spec 1.2: on Linux both modules' lws_* calls resolve to whichever copy loaded first, so process-global state is shared"),
		TEXT("lwsl_visible in VaRestTests' copy around FWebSocketsModule's InitWebSockets; UE dlopens modules RTLD_LOCAL (UnixPlatformProcess.cpp:107-148)"));
#if PLATFORM_LINUX
	FString OwnerFile = TEXT("unknown");
	Dl_info Info = {};
	if (dladdr(reinterpret_cast<void*>(&lws_create_context), &Info) != 0 && Info.dli_fname)
	{
		OwnerFile = FPaths::GetCleanFilename(UTF8_TO_TCHAR(Info.dli_fname));
	}
	// Review M-4: the engine module's own path, from its object's vtable pointer (the Itanium ABI puts it first, in the
	// module's .so), not a bare soname that depends on the search path
	FString EngineCopy = TEXT("the WebSockets module's object was not found in any loaded file");
	Dl_info ModuleInfo = {};
	void* EngineLib = dladdr(*reinterpret_cast<void**>(&Module), &ModuleInfo) != 0 && ModuleInfo.dli_fname
		? dlopen(ModuleInfo.dli_fname, RTLD_LAZY | RTLD_NOLOAD) : nullptr;
	if (EngineLib)
	{
		void* EngineSymbol = dlsym(EngineLib, "lws_create_context");
		EngineCopy = !EngineSymbol ? TEXT("the engine module exports no lws_create_context")
			: EngineSymbol == reinterpret_cast<void*>(&lws_create_context) ? TEXT("the engine module's lws_create_context is the same function")
			: TEXT("the engine module has its own copy at another address");
		// RTLD_NOLOAD took a reference: give it back
		dlclose(EngineLib);
	}
	const FString EngineFile = ModuleInfo.dli_fname ? FPaths::GetCleanFilename(UTF8_TO_TCHAR(ModuleInfo.dli_fname)) : FString(TEXT("(unknown)"));
	Fact(*this, TEXT("coexist.lws_symbol_owner"), EVerdict::Recorded,
		FString::Printf(TEXT("this module's lws_create_context is in %s; the WebSockets module is %s: %s"), *OwnerFile, *EngineFile, *EngineCopy),
		TEXT("WebSocket spec 1.2: libUnrealEditor-WebSockets.so exports 185 lws_* functions"),
		TEXT("dladdr of lws_create_context; dladdr of the FWebSocketsModule object's vtable, then dlopen(that path, RTLD_NOLOAD) + dlsym"));
#else
	Fact(*this, TEXT("coexist.lws_symbol_owner"), EVerdict::Unverified,
		TEXT("unverified here; release-pass item (Win64, Mac): separate DLL / two-level-namespace copies by construction"),
		TEXT("WebSocket spec 1.2"), TEXT("this platform"));
#endif

	// 1b. Review I-3: does creating a context re-allocate the OpenSSL ex-data index this LWS copy's verify callback reads?
	// A first context allocates it (in a fresh process it is 0 until then), so only the contexts after that one can show a
	// re-allocation (review minor 1). Rigs made one after another, no TLS connection in flight (decision 16)
#if PLATFORM_LINUX
	// W0 final review M-2: the weak reference is null when this LWS copy has no such global (a renamed or static symbol in a
	// later bundled LWS): nothing to read, so the fact says so instead of the link failing
	if (&openssl_websocket_private_data_index == nullptr)
	{
		Fact(*this, TEXT("coexist.context_create_reallocates_ssl_index"), EVerdict::Unverified,
			TEXT("unverified here: symbol not present in this LWS copy (the weak reference to openssl_websocket_private_data_index is null)"),
			TEXT("WebSocket spec 1.2: \"Contexts are independent. All connection state lives in the lws_context\""),
			TEXT("a weak reference to LWS's private index global; for LWS 3.0 the static evidence is ssl.c.o 0x1f4, 0x23a and openssl-client.c.o 0x1c8-0x1d6 (review I-3)"));
	}
	else
	{
		const int32 IndexBefore = openssl_websocket_private_data_index;
		int32 IndexAfterFirst = IndexBefore;
		int32 IndexAfterPlain = IndexBefore;
		int32 IndexAfterTls = IndexBefore;
		{
			FRig FirstRig(Site);
			if (!TestTrue(TEXT("the first probe context exists"), FirstRig.IsReady()))
			{
				return false;
			}
			IndexAfterFirst = openssl_websocket_private_data_index;
		}
		{
			FRig PlainRig(Site);
			if (!TestTrue(TEXT("the plain probe context exists"), PlainRig.IsReady()))
			{
				return false;
			}
			IndexAfterPlain = openssl_websocket_private_data_index;
		}
		{
			FRigOptions TlsOptions;
			TlsOptions.Tls = ETls::LwsGlobalInit;
			FRig TlsRig(Site, MoveTemp(TlsOptions));
			if (!TestTrue(TEXT("the TLS probe context exists"), TlsRig.IsReady()))
			{
				return false;
			}
			IndexAfterTls = openssl_websocket_private_data_index;
		}
		Dl_info IndexInfo = {};
		const FString IndexFile = dladdr(reinterpret_cast<void*>(&openssl_websocket_private_data_index), &IndexInfo) != 0 && IndexInfo.dli_fname
			? FPaths::GetCleanFilename(UTF8_TO_TCHAR(IndexInfo.dli_fname)) : FString(TEXT("(unknown)"));
		// Differs only when a later creation changed the index the first one had already allocated
		Fact(*this, TEXT("coexist.context_create_reallocates_ssl_index"), AgreesIf(IndexAfterFirst == IndexAfterPlain && IndexAfterPlain == IndexAfterTls),
			FString::Printf(TEXT("openssl_websocket_private_data_index (in %s) was %d before this test's contexts, %d after a first plain context (allocated), then %d after a second plain context and %d after a TLS context (DO_SSL_GLOBAL_INIT)"),
				*IndexFile, IndexBefore, IndexAfterFirst, IndexAfterPlain, IndexAfterTls),
			TEXT("WebSocket spec 1.2: \"Contexts are independent. All connection state lives in the lws_context\""),
			TEXT("the global read around three lws_create_context calls in VaRestTests' copy, the verdict from the second and third; statically, lws_context_init_ssl_library stores a new CRYPTO_get_ex_new_index on both paths (ssl.c.o 0x1f4, 0x23a) and OpenSSL_client_verify_callback dereferences SSL_get_ex_data under it without a null check (openssl-client.c.o 0x1c8-0x1d6): a TLS handshake in flight across a context creation in the same copy reads NULL"));
	}
#else
	Fact(*this, TEXT("coexist.context_create_reallocates_ssl_index"), EVerdict::Unverified,
		TEXT("unverified here (static evidence only); release-pass item (Win64, Mac): the global is read on Linux only"),
		TEXT("WebSocket spec 1.2: \"Contexts are independent. All connection state lives in the lws_context\""),
		TEXT("ssl.c.o 0x1f4, 0x23a and openssl-client.c.o 0x1c8-0x1d6 (review I-3)"));
#endif

	// Steps 2-4 run with the engine's certificate check off; the block's end restores it, on every path out of the block
	{
		// Decision 11 (ruling Q4): the engine's leg skips its certificate check for this test only, and connects only to the
		// local test server while it does. The flag leaves wss at LCCSCF_ALLOW_SELFSIGNED (LwsWebSocket.cpp:741-743); the
		// verify callback then sets X509_V_OK and accepts (:554-568), which is what lets an IP URL past the host-name check
		// (review M-9). The engine reads the key again on a config change (LwsWebSocketsManager.cpp:91-93, WebSocketsModule.cpp:47, :68-74).
		// The restore is armed before anything changes, so every exit path runs it (review M-3)
		bool bWasDisabled = false;
		const bool bHadKey = GConfig->GetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), bWasDisabled, GEngineIni);
		const FString OldVerbosity = LogVerbosityOf(TEXT("LogWebSockets"));
		ON_SCOPE_EXIT
		{
			// The old value goes back first and the engine reads it. Its re-read assigns the flag only when the key is there
			// (GetBool leaves its argument alone otherwise, ConfigCacheIni.cpp:5424-5438), so removing a key that was absent
			// straight away would leave the engine's flag on for the rest of the process. Absent before, bWasDisabled is
			// still false here: the engine's default (LwsWebSocketsManager.h:138)
			GConfig->SetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), bWasDisabled, GEngineIni);
			FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, TSet<FString>{ TEXT("LwsWebSocket") });
			if (!bHadKey)
			{
				// Absent before: removed again once the engine has read the old value, never left in the host's ini as False
				GConfig->RemoveKey(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), GEngineIni);
			}
#if UE_ALLOW_EXEC_COMMANDS
			if (!OldVerbosity.IsEmpty())
			{
				if (GEngine)
				{
					// The verbosity read above, not LogWebSockets' declared one
					GEngine->Exec(nullptr, *FString::Printf(TEXT("Log LogWebSockets %s"), *OldVerbosity));
				}
				else
				{
					AddWarning(FString::Printf(TEXT("GEngine is null at the restore: LogWebSockets is not put back to %s"), *OldVerbosity));
				}
			}
#endif
		};
		GConfig->SetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), true, GEngineIni);
		FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, TSet<FString>{ TEXT("LwsWebSocket") });
#if UE_ALLOW_EXEC_COMMANDS
		// UEngine::Exec exists where exec commands are allowed (Engine.h:2434-2438, Misc/Exec.h:11-15); "Log" is handled by
		// the log suppression's Exec_Runtime (LogSuppressionInterface.cpp:589-591). Only when the old verbosity was read,
		// so that the restore can put it back
		if (!GEngine)
		{
			AddWarning(TEXT("GEngine is null: LogWebSockets' verbosity is neither read, raised nor restored (no UEngine::Exec)"));
		}
		else if (!OldVerbosity.IsEmpty())
		{
			GEngine->Exec(nullptr, TEXT("Log LogWebSockets Verbose"));
		}
#endif
		AddInfo(FString::Printf(TEXT("[LwsWebSocket] bDisableCertValidation before this test: %s; LogWebSockets verbosity before: %s"),
			bHadKey ? (bWasDisabled ? TEXT("True") : TEXT("False")) : TEXT("(absent)"), OldVerbosity.IsEmpty() ? TEXT("(not read)") : *OldVerbosity));

		// 2. ws and wss, an engine socket and a probe connection open at once
		FString WsWhy;
		FString WssWhy;
		bool bWs = false;
		bool bWss = false;
		{
			FRigOptions Options;
			Options.Tls = ETls::LwsGlobalInit;
			FRig Rig(Site, MoveTemp(Options));
			if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
			{
				return false;
			}
			const TSharedRef<FEngineSide> EngineWs = MakeShared<FEngineSide>();
			const TSharedRef<IWebSocket> EngineWsSocket = OpenEngineSocket(Module, FString::Printf(TEXT("ws://%s:%d/ws/echo?key=%s"), *Site.Host, Site.Port, *NewKey()), EngineWs);
			FConnectSpec Plain;
			Plain.Path = TEXT("/ws/echo?key=") + NewKey();
			bWs = BothEcho(Rig, Rig.Connect(MoveTemp(Plain)), EngineWsSocket, EngineWs, WsWhy);

			const TSharedRef<FEngineSide> EngineWss = MakeShared<FEngineSide>();
			const TSharedRef<IWebSocket> EngineWssSocket = OpenEngineSocket(Module, FString::Printf(TEXT("wss://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *NewKey()), EngineWss);
			bWss = BothEcho(Rig, Rig.Connect(Tls(Site, TEXT("localhost"), LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK)), EngineWssSocket, EngineWss, WssWhy);
		}
		Fact(*this, TEXT("coexist.both_ws"), AgreesIf(bWs), WsWhy,
			TEXT("WebSocket spec 1.2: an engine IWebSocket and a VaRest-owned LWS context work at once (ws)"), TEXT("one engine socket and one probe connection to /ws/echo at once"));
		Fact(*this, TEXT("coexist.both_wss"), AgreesIf(bWss), WssWhy,
			TEXT("WebSocket spec 1.2: the same over TLS (wss)"), TEXT("the same to the TLS listener; the probe context with DO_SSL_GLOBAL_INIT"));

		// 3. The probe's context, TLS included, is gone (lws_context_destroy with DO_SSL_GLOBAL_INIT): the engine still opens wss
		const TSharedRef<FEngineSide> Later = MakeShared<FEngineSide>();
		const TSharedRef<IWebSocket> LaterSocket = OpenEngineSocket(Module, FString::Printf(TEXT("wss://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *NewKey()), Later);
		VaRest::Http::Testing::PumpUntil([Later] { return Later->bConnected || Later->bEnded; }, 20.0, true);
		bool bLaterEchoed = false;
		if (Later->bConnected)
		{
			LaterSocket->Send(TEXT("after"));
			bLaterEchoed = VaRest::Http::Testing::PumpUntil([Later] { return EngineEchoed(Later, TEXT("after")); }, 20.0, true);
			LaterSocket->Close(1000, TEXT("done"));
		}
		const bool bLaterEnded = VaRest::Http::Testing::PumpUntil([Later] { return Later->bEnded || !Later->bConnected; }, 20.0, true);
		Fact(*this, TEXT("coexist.engine_after_probe_teardown"), AgreesIf(Later->bConnected && bLaterEchoed && bLaterEnded),
			FString::Printf(TEXT("engine wss after the probe's context was destroyed: connected=%s echoed=%s ended=%s error=\"%s\""),
				YesNo(Later->bConnected), YesNo(bLaterEchoed), YesNo(bLaterEnded), *Later->Error),
			TEXT("WebSocket spec 1.2: VaRest's context teardown leaves the engine's TLS working"), TEXT("a new engine wss socket after the rig's Shutdown"));

		// 4. The engine's LWS lines still reach LogWebSockets through its own emitter
		int32 LwsLines = 0;
		for (const FString& Line : Capture.GetLines())
		{
			LwsLines += Line.StartsWith(TEXT("LogWebSockets: ")) && Line.Contains(TEXT("Lws(")) ? 1 : 0;
		}
		Fact(*this, TEXT("coexist.engine_lws_log_routed"), EVerdict::Recorded,
			FString::Printf(TEXT("%d libwebsockets lines in LogWebSockets during this test (Verbose on)"), LwsLines),
			TEXT("WebSocket spec 1.2 and D30: the engine's routing to LogWebSockets (LwsWebSocketsManager.cpp:174-176) stays; VaRest never calls lws_set_log_level when WebSockets is loaded"),
			TEXT("FLogCapture over the engine sockets above"));
	}

	// 5. Harness (ruling Q4): the restore took. The engine's flag is private (LwsWebSocketsManager.h:138), so it is read from
	// what the engine does: with its check on again, a wss to the test server is refused (the test CA is not among the
	// engine's roots, and LWS fails 127.0.0.1's host-name check, tls.insecure_ip_host); with the flag left on, it opens.
	// -lws-insecure= on the command line overrides the ini on every re-read outside Shipping (LwsWebSocketsManager.cpp:95-98,
	// FParse::Bool as here): then the flag is the command line's before, during and after this test, and an engine wss
	// cannot show whether the restore took, so the check is skipped and says why (review minor 5)
	bool bInsecureArg = false;
	bool bHasInsecureArg = false;
#if !UE_BUILD_SHIPPING
	bHasInsecureArg = FParse::Bool(FCommandLine::Get(), TEXT("lws-insecure="), bInsecureArg);
#endif
	if (bHasInsecureArg)
	{
		AddWarning(FString::Printf(TEXT("the restore check is skipped: -lws-insecure=%s on the command line sets the engine's flag on every config re-read (LwsWebSocketsManager.cpp:95-98), so an engine wss cannot show whether the restore took"),
			bInsecureArg ? TEXT("true") : TEXT("false")));
	}
	else
	{
		const TSharedRef<FEngineSide> Checked = MakeShared<FEngineSide>();
		const TSharedRef<IWebSocket> CheckedSocket = OpenEngineSocket(Module, FString::Printf(TEXT("wss://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *NewKey()), Checked);
		VaRest::Http::Testing::PumpUntil([Checked] { return Checked->bConnected || Checked->bEnded; }, 20.0, true);
		const bool bCheckOpened = Checked->bConnected;
		if (Checked->bConnected && !Checked->bEnded)
		{
			CheckedSocket->Close(1000, TEXT("done"));
		}
		const bool bCheckEnded = VaRest::Http::Testing::PumpUntil([Checked] { return Checked->bEnded; }, 20.0, true);
		TestTrue(FString::Printf(TEXT("the engine's certificate check is on again after the restore: its wss to the test server is refused (connected=%s ended=%s error=\"%s\")"),
			YesNo(bCheckOpened), YesNo(bCheckEnded), *Checked->Error), !bCheckOpened && bCheckEnded);
	}
	// A leg that did not echo is a fact (coexist.both_ws / both_wss say which side), not a failure (review M-2)
	return true;
#else
	Fact(*this, TEXT("coexist.shared_lws_state"), EVerdict::Unverified, TEXT("unverified here: WITH_WEBSOCKETS is 0 on this platform"), TEXT("WebSocket spec 1.2"), TEXT("this platform"));
	Fact(*this, TEXT("coexist.lws_symbol_owner"), EVerdict::Unverified, TEXT("unverified here: WITH_WEBSOCKETS is 0"), TEXT("WebSocket spec 1.2"), TEXT("this platform"));
	Fact(*this, TEXT("coexist.both_ws"), EVerdict::Unverified, TEXT("unverified here: WITH_WEBSOCKETS is 0"), TEXT("WebSocket spec 1.2"), TEXT("this platform"));
	Fact(*this, TEXT("coexist.both_wss"), EVerdict::Unverified, TEXT("unverified here: WITH_WEBSOCKETS is 0"), TEXT("WebSocket spec 1.2"), TEXT("this platform"));
	Fact(*this, TEXT("coexist.engine_after_probe_teardown"), EVerdict::Unverified, TEXT("unverified here: WITH_WEBSOCKETS is 0"), TEXT("WebSocket spec 1.2"), TEXT("this platform"));
	Fact(*this, TEXT("coexist.engine_lws_log_routed"), EVerdict::Unverified, TEXT("unverified here: WITH_WEBSOCKETS is 0"), TEXT("WebSocket spec 1.2"), TEXT("this platform"));
	Fact(*this, TEXT("coexist.context_create_reallocates_ssl_index"), EVerdict::Unverified, TEXT("unverified here: WITH_WEBSOCKETS is 0"), TEXT("WebSocket spec 1.2"), TEXT("this platform"));
	return true;
#endif
}

#endif // WITH_DEV_AUTOMATION_TESTS
