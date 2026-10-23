// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "Misc/SingleThreadRunnable.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"
#include "VaRestLwsInclude.h"

#include <atomic>

class FRunnableThread;
class ISslCertificateManager;
class ISslManager;

namespace VaRest::WebSocket::Lws
{
	class FService;

	/** LWS's own close-ack wait after our close frame, fixed in 3.0 (T20: ops-ws.c.o rops_handle_POLLOUT_ws 0x103c-0x104a) */
	inline constexpr double LwsCloseAckSeconds = 5.0;
	/**
	 * A pending kill that has not landed this long after it was armed is armed again (review conc I-1, re-review N-1): one
	 * timeout walk -- LWS walks only when the wall-clock second changed (T26) -- plus a margin. Shorter re-arms postpone the kill
	 */
	inline constexpr double KillRearmSeconds = 1.25;
	/** A message goes out in fragments of at most this many bytes (spec 2.4, 15) */
	inline constexpr int32 FragmentBytes = 64 * 1024;
	/** The one protocol's receive buffer, the engine's (LwsWebSocketsManager.cpp:166; spec 2.1) */
	inline constexpr int32 RxBufferBytes = 64 * 1024;
	/** The handshake header buffer, the engine's (LwsWebSocketsManager.cpp:187-190; spec 15) */
	inline constexpr int32 HeaderBufferBytes = 32 * 1024;

	struct FServiceOptions
	{
		/** lws_service's wait with no connection: commands wake it (lws_cancel_service) */
		int32 IdleWaitMs = 30000;
		/** ... with connections: LWS's own timers run only when lws_service returns (spec 2.1, T26) */
		int32 ActiveWaitCapMs = 1000;
		/** ... while a kill is in flight or LWS holds a partial write (plan decision 12, VaRest-wcv) */
		int32 ShortWaitCapMs = 100;
		/** Tests: PEM certificates trusted on top of the engine's roots (the test CA) */
		TArray<uint8> ExtraTrustedPem;
		/** Tests: every connection's TCP goes here; the URL's host stays the Host header and the TLS name (plan decision 19) */
		FString TcpAddressOverride;
		/** Tests: the URL filter's map instead of the engine ini (plan decision 19); read by FConn::Prepare */
		bool bOverrideAllowlist = false;
		TArray<TPair<FString, TArray<FString>>> Allowlist;
		FString ThreadName = TEXT("VaRestLws");
		/**
		 * Shutdown first lets the running thread write the close frames already asked for, at most this long (review conc
		 * M-3): at engine exit the hub's 1001 closes (spec 1.4) are queued by the game instances' shutdown
		 * (GameEngine.cpp:1377, :1391) just before OnEnginePreExit (:1396, UnrealEngine.cpp:2709) shuts the transport down
		 */
		double ExitFlushSeconds = 0.25;
		/** Tests (review conc I-2): Start makes the context, then fails as if the thread could not be made */
		bool bFailThreadForTest = false;
	};

	/** What a test reads (any thread) */
	struct FServiceCounters
	{
		/** Connections the service holds: adopted and not yet destroyed by LWS */
		std::atomic<int32> LiveConnections{ 0 };
		std::atomic<int32> KillsIssued{ 0 };
		std::atomic<int32> KillsLanded{ 0 };
		std::atomic<int32> CommandsRun{ 0 };
		/** lws_create_context calls this service made, failed ones included (review conc I-2) */
		std::atomic<int32> ContextCreations{ 0 };
		/** Open connections whose close was asked for and not written yet: Shutdown's flush waits for 0 (review conc M-3) */
		std::atomic<int32> UnsentCloses{ 0 };
		/** Seconds from lws_set_timeout(LWS_TO_KILL_ASYNC) to WSI_DESTROY; -1 before the first (VaRest-wcv) */
		std::atomic<double> LastKillToDestroySeconds{ -1.0 };
		std::atomic<double> MaxKillToDestroySeconds{ -1.0 };
	};

	/** One connection as the service sees it: FConn (Task 7), or a test's stand-in. Service thread, except EndForShutdown */
	class IServiceConnection
	{
	public:
		virtual ~IServiceConnection() = default;
		virtual uint64 GetId() const = 0;
		/** Right after Adopt: connects, or reports why it cannot */
		virtual void Start(FService& Service) = 0;
		/** A libwebsockets callback the service matched to this connection by its userdata */
		virtual int32 OnCallback(FService& Service, lws* InWsi, int32 Reason, void* User, void* In, size_t Len) = 0;
		/** After each return of lws_service, outside any callback */
		virtual void CheckDeadlines(FService& Service, double Now) = 0;
		/** The earliest FPlatformTime::Seconds() at which CheckDeadlines has work; TNumericLimits<double>::Max() for none */
		virtual double NextDeadline() const = 0;
		/** A kill in flight or a partial write held: the wait is capped short (plan decision 12) */
		virtual bool WantsShortWait() const = 0;
		/** LWS is done with it (WSI_DESTROY, a scrubbed connect, or never connected): the service erases it */
		virtual bool IsDestroyed() const = 0;
		/** Shutdown, on the game thread with the service thread stopped: queue the terminal event if there is none yet */
		virtual void EndForShutdown() = 0;
	};

	using FCommand = TUniqueFunction<void(FService&)>;

	/**
	 * One lws_context and the thread that services it (WebSocket spec 2.1). The thread waits in lws_service, runs commands
	 * inside LWS_CALLBACK_EVENT_WAIT_CANCELLED (T13, T24), and checks VaRest's own deadlines after each return. Only
	 * lws_cancel_service is called from another thread; the context is made and destroyed on the game thread with the
	 * thread stopped (LwsWebSocketsManager.cpp:231, :298-309). A service starts once: after Shutdown it never runs again.
	 */
	class FService final : public FRunnable, public FSingleThreadRunnable
	{
	public:
		explicit FService(FServiceOptions InOptions);
		virtual ~FService() override;
		FService(const FService&) = delete;
		FService& operator=(const FService&) = delete;

		/**
		 * Game thread: the context and the thread; false (logged) when either could not be made -- and from then on, without
		 * another lws_create_context (review conc I-2: in a shared LWS copy a retry could run under an engine wss handshake,
		 * VaRest-rti). False, logged and not latched, while TlsHandshakesInFlight() is not 0 (review conc I-3)
		 */
		bool Start();
		/**
		 * Game thread: lets the thread write the close frames already asked for (at most ExitFlushSeconds), stops it, runs the
		 * queued commands, ends every connection (EndForShutdown), destroys the context
		 */
		void Shutdown();
		/** Any thread */
		bool IsRunning() const { return bAccepting.load(); }
		/** Any thread: Shutdown has begun (FConn::Start reports Shutdown then) */
		bool IsShuttingDown() const { return bShuttingDown.load(); }
		/**
		 * Any thread: Command runs on the service thread. True when it was queued; false when the service does not accept
		 * (never started, its start failed, or Shutdown has begun), and the command is released before this returns, its
		 * captures with it (review conc M-7): nothing would ever run it
		 */
		bool Enqueue(FCommand&& Command);

		// ---- service thread (or the game thread while the thread does not run)
		lws_context* GetContext() const { return Context; }
		const FServiceOptions& GetOptions() const { return Options; }
		ISslCertificateManager* GetCertificates() const { return Certificates; }
		FServiceCounters& GetCounters() { return Counters; }
		/**
		 * Takes the connection, makes its userdata known, and starts it. Its id is unique among the connections held. The
		 * userdata it gives LWS (lws_client_connect_info::userdata) is this IServiceConnection pointer itself, not a derived
		 * class's: FindForCallback matches LWS's userdata against these pointers by address
		 */
		void Adopt(TUniquePtr<IServiceConnection>&& Connection);
		IServiceConnection* Find(uint64 Id) const;
		/** lws_service's next wait (plan decision 12); public for the service's tests */
		int32 ComputeWaitMs(double Now) const;

		/** The protocol's callback (lws_protocols::callback): the service comes from the context's user pointer */
		static int Callback(lws* Wsi, lws_callback_reasons Reason, void* User, void* In, size_t Len);

	private:
		virtual uint32 Run() override;
		virtual void Stop() override;
		virtual FSingleThreadRunnable* GetSingleThreadInterface() override { return this; }
		/** FSingleThreadRunnable: where there is no multithreading, the main thread ticks the forkable thread (Fork.h:86-93) */
		virtual void Tick() override;
		void ServiceOnce(int32 WaitMs);
		/** Game thread, the thread running: until every close asked for before the call is written, or Seconds pass (review conc M-3) */
		void FlushCloses(double Seconds);
		void DrainCommands();
		void CheckDeadlines(double Now);
		void EraseDestroyed();
		int32 Handle(lws* Wsi, int32 Reason, void* User, void* In, size_t Len);
		IServiceConnection* FindForCallback(lws* Wsi, void* User, int32 Reason) const;
		void LoadClientCertificates(SSL_CTX* Ctx);
		void AddExtraTrusted(SSL_CTX* Ctx) const;
		void ReleaseSsl();

		FServiceOptions Options;
		lws_context* Context = nullptr;
		lws_protocols Protocols[2] = {};
		FRunnableThread* Thread = nullptr;
		std::atomic<bool> bStopping{ false };
		std::atomic<bool> bAccepting{ false };
		std::atomic<bool> bShuttingDown{ false };
		/** Game thread: a Start failed; Start never tries again (review conc I-2) */
		bool bStartFailed = false;
		/**
		 * Held by Enqueue around its check and lws_cancel_service, and by Start and Shutdown when they change bAccepting: the
		 * context is never destroyed under an Enqueue -- lws_cancel_service is safe from another thread only "while the
		 * context exists" (libwebsockets.h:3712-3713). Nothing under it waits: the cancel writes LWS's cancel pipe, made
		 * O_NONBLOCK (the Release archive's lws_plat_pipe_create; review conc, "Checked and sound" 1); the callback it causes
		 * comes later, in the service loop (libwebsockets.h:3707-3710)
		 */
		FCriticalSection EnqueueLock;
		TQueue<FCommand, EQueueMode::Mpsc> Commands;
		TMap<uint64, TUniquePtr<IServiceConnection>> Connections;
		/** The userdata pointers LWS may hand back: a callback whose userdata is not here belongs to no live connection */
		TSet<const IServiceConnection*> Live;
		ISslCertificateManager* Certificates = nullptr;
		ISslManager* SslManager = nullptr;
		SSL_CTX* ProvidedSslCtx = nullptr;
		bool bSslInitialized = false;
		TArray<ANSICHAR> ProxyUtf8;
		FServiceCounters Counters;
	};

	/** Any thread: Text as NUL-terminated UTF-8 in Out, for the char* fields libwebsockets keeps */
	void Utf8Copy(const FString& Text, TArray<ANSICHAR>& Out);

	/** Game thread: the D30 rule for this module's LWS copy (spec 1.2; plan decision 16) */
	void ApplyLogLevel();

	/** Plan decision 8 (VaRest-rti): whether VaRest's context is made at module startup */
	bool ShouldCreateAtStartup(bool bSharedCopy, TFunctionRef<bool(const TCHAR*)> IsLinked);

	/**
	 * Any thread: VaRest's wss attempts, across every service of this module, that LWS has neither established
	 * (CLIENT_ESTABLISHED) nor destroyed (WSI_DESTROY) -- raised by FTransport::Connect on the game thread, where
	 * FService::Start reads it (re-review N-2), lowered once by the FConn (review conc I-3, VaRest-rti).
	 * Every lws_create_context re-allocates this LWS copy's OpenSSL ex-data index that their verify callbacks read
	 * unchecked [coexist.context_create_reallocates_ssl_index], so FService::Start refuses while it is not 0
	 */
	std::atomic<int32>& TlsHandshakesInFlight();
}
