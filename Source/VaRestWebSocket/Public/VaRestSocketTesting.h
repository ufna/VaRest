// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestSocketTransport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Test hooks for VaRest's libwebsockets transport (WebSocket spec 16.2). Compiled with WITH_DEV_AUTOMATION_TESTS only: not
 * for games. One rule for every test (VaRest-rti; the W0 probe's decision 16; review conc I-3): never make a transport while
 * another transport of this module has a wss attempt that LWS has not destroyed yet -- an attempt that has ended still runs
 * its handshake until LWS destroys it, so wait for that transport's LiveConnections to reach 0. Each transport makes an LWS
 * context, and every context creation re-allocates this LWS copy's OpenSSL ex-data index under a handshake in flight
 * [coexist.context_create_reallocates_ssl_index]. The rule is enforced by shape too: a transport's first connect makes no
 * context while GetTlsHandshakesInFlight() is not 0, and ends with Shutdown instead.
 */
namespace VaRest::WebSocket::Testing
{
	struct FLwsTransportOptions
	{
		/** PEM certificates trusted on top of the engine's roots: the test CA (Tools/testserver/certs/test-ca.pem, GET /tls/ca.pem) */
		TArray<uint8> ExtraTrustedPem;
		/** Non-empty: every connection's TCP goes here; the URL's host stays the Host header and the TLS name (the probe's FConnectSpec::Address) */
		FString TcpAddressOverride;
		/** true: the URL filter is Allowlist (scheme -> domains, URLRequestFilter.h:16-27), not [Online.HttpManager <scheme>] in the engine ini */
		bool bOverrideAllowlist = false;
		TArray<TPair<FString, TArray<FString>>> Allowlist;
		/** lws_service's cap while connections exist (spec 2.1: 1000 ms), and while a kill is in flight (plan decision 12: 100 ms) */
		int32 ActiveWaitCapMs = 1000;
		int32 ShortWaitCapMs = 100;
		/** Shutdown waits at most this long for the close frames already asked for (review conc M-3) */
		double ExitFlushSeconds = 0.25;
		/** The first connect makes the context, then fails as if the thread could not start (review conc I-2) */
		bool bFailThreadForTest = false;
	};

	/** A snapshot of a transport's counters */
	struct FLwsCounters
	{
		/** Connections LWS has not destroyed yet */
		int32 LiveConnections = 0;
		int32 KillsIssued = 0;
		int32 KillsLanded = 0;
		/** lws_create_context calls, failed ones included (review conc I-2) */
		int32 ContextCreations = 0;
		/** Closes asked for on open connections and not written yet (review conc M-3) */
		int32 UnsentCloses = 0;
		/** Seconds from lws_set_timeout(LWS_TO_KILL_ASYNC) to WSI_DESTROY: the last, and the longest so far; -1 before the first (VaRest-wcv) */
		double LastKillToDestroySeconds = -1.0;
		double MaxKillToDestroySeconds = -1.0;
	};

	class ILwsTestTransport : public IVaRestSocketTransport
	{
	public:
		/** Game thread: ends every connection (each gets a terminal event; Failure Shutdown when it had none), stops the thread, destroys the context. The destructor calls it too */
		virtual void Shutdown() = 0;
		/** Game thread */
		virtual FLwsCounters GetCounters() const = 0;
	};

	/** Game thread: a transport with a context and a service thread of its own, started at its first Connect */
	VARESTWEBSOCKET_API TSharedRef<ILwsTestTransport, ESPMode::ThreadSafe> MakeLwsTransport(FLwsTransportOptions Options);

	/** Linux: the file that holds this module's lws_create_context (dladdr); elsewhere "(not on this platform)" (VaRest-5ia M-1) */
	VARESTWEBSOCKET_API FString DescribeLwsCopy();
	/** This module's lws_create_context, to compare with another module's copy */
	VARESTWEBSOCKET_API const void* GetLwsCreateContextAddress();
	/** lwsl_visible for this module's LWS copy: "err=1 warn=2 notice=0 info=0 client=0" (libwebsockets.h:434-443) */
	VARESTWEBSOCKET_API FString DescribeLwsLogLevel();

	/** Any thread: this module's wss attempts that LWS has neither established nor destroyed, across every transport (review conc I-3) */
	VARESTWEBSOCKET_API int32 GetTlsHandshakesInFlight();

	/** Game thread: counts as one wss attempt in flight while it lives, so no transport of this module makes a context (review conc I-3) */
	class VARESTWEBSOCKET_API FScopedTlsHandshakeInFlight
	{
	public:
		FScopedTlsHandshakeInFlight();
		~FScopedTlsHandshakeInFlight();
		FScopedTlsHandshakeInFlight(const FScopedTlsHandshakeInFlight&) = delete;
		FScopedTlsHandshakeInFlight& operator=(const FScopedTlsHandshakeInFlight&) = delete;
	};
}

#endif // WITH_DEV_AUTOMATION_TESTS
