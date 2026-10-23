// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestJson.h"
#include "VaRestSocketTesting.h"
#include "VaRestSocketTransport.h"

#if WITH_DEV_AUTOMATION_TESTS

#include <atomic>

class FAutomationTestBase;

/** Helpers for VaRest.Integration.WebSocket.Transport.* (WebSocket spec 16.2) against Tools/testserver/ws.py */
namespace VaRest::WsTest
{
	/** Test-only OnEvents hold: deliberately blocks its producer until released, at most 10 seconds. Shared captures only. */
	struct FCallbackGate
	{
		std::atomic<bool> bEntered{ false };
		std::atomic<bool> bRelease{ false };
		std::atomic<bool> bCompleted{ false };
		std::atomic<bool> bTimedOut{ false };
		/** Producer thread. */
		void Hold();
		/** Game thread: bounded waits, pumping callbacks. */
		bool WaitEntered();
		bool ReleaseAndWait();
	};

	/** The runner's test server: -VaRestTestServer=http://127.0.0.1:<port>, -VaRestTestServerTls=https://127.0.0.1:<port> */
	struct FSite
	{
		FString HttpBase;
		FString Host;
		int32 Port = 0;
		int32 TlsPort = 0;
		/** -VaRestTestServerSilent=tcp://127.0.0.1:<port>: accepts, reads and discards, never writes (Task 11 review I-1); 0 when absent */
		int32 SilentPort = 0;
	};

	/** AddError and false when the runner did not pass the server or (bNeedTls) its TLS listener: an integration test fails, never skips */
	bool FindSite(FAutomationTestBase& Test, FSite& Out, bool bNeedTls = true);

	/** AddError and false when the runner did not pass the silent listener (Tools/testserver/server.py --silent-port): a test fails, never skips */
	bool HasSilentListener(FAutomationTestBase& Test, const FSite& Site);

	/** One way to reach the server: ws://127.0.0.1:<port>, or wss://localhost:<tls port> (the name checked against the test CA, TCP to 127.0.0.1) */
	struct FScheme
	{
		const TCHAR* Name = TEXT("ws");
		FString Base;
		bool bTls = false;
	};
	TArray<FScheme> Schemes(const FSite& Site);

	FString NewKey();

	/**
	 * A transport of its own -- context and thread -- with TCP to the server's host (plan decision 19) and, with bTrustTestCa,
	 * the test CA trusted (GET /tls/ca.pem, fetched once). Null, with an AddError, when the CA could not be fetched. Shut it
	 * down before the test returns (ON_SCOPE_EXIT), and never make one while another transport still has a wss attempt that
	 * LWS has not destroyed (its LiveConnections back to 0): an ended attempt still runs its handshake (VaRest-rti; re-review N-5).
	 * BaseOptions carries the rest (a test switch); its TcpAddressOverride and ExtraTrustedPem are set here.
	 */
	TSharedPtr<VaRest::WebSocket::Testing::ILwsTestTransport, ESPMode::ThreadSafe> MakeTransport(FAutomationTestBase& Test, const FSite& Site, bool bTrustTestCa = true,
		VaRest::WebSocket::Testing::FLwsTransportOptions BaseOptions = {});

	/** VaRest's libwebsockets lines are Warnings (plan decision 16); bElevateLogWarningsToErrors defaults to true (AutomationControllerSettings.cpp:14) */
	void ExpectLwsLines(FAutomationTestBase& Test);

	/** A connection's events, popped as the game thread is pumped; Message bytes are acknowledged (Consumed) as they are popped */
	class FEvents
	{
	public:
		explicit FEvents(TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> InConnection);

		/** Pumps the game thread (and the core ticker) until Done holds over the events so far, or Seconds pass */
		bool WaitFor(TFunctionRef<bool(const TArray<FVaRestSocketEvent>&)> Done, double Seconds = 15.0);
		bool WaitType(EVaRestSocketEventType Type, double Seconds = 15.0);
		/** Until the terminal event (Closed or Error) */
		bool WaitEnd(double Seconds = 15.0);
		/** Until Wanted messages, or the end; true when Wanted arrived */
		bool WaitMessages(int32 Wanted, double Seconds = 15.0);
		/** Pops what is queued now, without pumping */
		void Pull();

		const FVaRestSocketEvent* Find(EVaRestSocketEventType Type) const;
		/** The terminal event, or null */
		const FVaRestSocketEvent* GetEnd() const;
		TArray<const FVaRestSocketEvent*> GetMessages() const;
		int32 Count(EVaRestSocketEventType Type) const;
		const TArray<FVaRestSocketEvent>& GetAll() const { return Seen; }
		/** "Open > Message x3 > Closed 1000 by Client" for a test's messages */
		FString Describe() const;

		/** false: the test acknowledges messages itself (flow control tests, W1b) */
		bool bAcknowledge = true;
		TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection;

	private:
		TArray<FVaRestSocketEvent> Seen;
	};

	/** Connects, waits for the open or the end, closes 1000 when it opened, waits for the end: every connection drains */
	TUniquePtr<FEvents> Visit(IVaRestSocketTransport& Transport, FVaRestSocketConnectRequest&& Req, double Seconds = 15.0);

	/** The connection ended with Error and this failure */
	bool IsError(const FEvents& Events, EVaRestSocketFailure Failure);

	FVaRestSocketConnectRequest Request(const FScheme& Scheme, const FString& PathAndQuery);

	/** GET /ws/journal over HTTP; an invalid handle and an AddError when it did not arrive */
	FVaRestJson FetchJournal(FAutomationTestBase& Test, const FSite& Site, const FString& Key, int32 Connections = 1, int32 WaitMs = 5000);
	/** GET Path as JSON over HTTP within Seconds; an invalid handle and an AddError when it did not arrive */
	FVaRestJson FetchJson(FAutomationTestBase& Test, const FSite& Site, const FString& Path, double Seconds = 15.0);
	bool FetchBytes(FAutomationTestBase& Test, const FSite& Site, const FString& Path, TArray<uint8>& Out);
	/** The journal's events of one kind ("ev"), of one connection (1-based) or all (0) */
	TArray<FVaRestJson> JournalEvents(const FVaRestJson& Journal, const TCHAR* Kind, int32 ConnIndex = 0);
	/** A request header the server received (name compared ignoring case), or "(absent)" */
	FString JournalHeader(const FVaRestJson& Journal, const TCHAR* Name, int32 ConnIndex = 1);
	/** The code of the first close frame the server received from the client (-1: none; the code is absent for an empty payload), and its reason */
	int32 JournalCloseCode(const FVaRestJson& Journal, FString* OutReason = nullptr, int32 ConnIndex = 1);

	FString Utf8(TConstArrayView<uint8> Bytes);
	FString Sha256Hex(TConstArrayView<uint8> Bytes);
}

#endif // WITH_DEV_AUTOMATION_TESTS
