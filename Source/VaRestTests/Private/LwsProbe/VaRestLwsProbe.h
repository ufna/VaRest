// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

// WebSocket spec 16.4 (W0): a minimal harness that drives the engine-bundled libwebsockets 3.0 directly -- no VaRest
// client code -- against Tools/testserver/ws.py, and logs what it sees as FACT lines for Tools/lws_probe_passport.py.
// One FRig per test: one lws_context with one protocol "varest", one service thread that waits in lws_service and is
// woken by lws_cancel_service, game-thread commands run inside LWS_CALLBACK_EVENT_WAIT_CANCELLED (the shape of WebSocket
// spec 2.1), and every callback recorded in order. Test-only: VaRestTests is UncookedOnly, never in a game target.
//
// One constraint for every test (WS W0 decision 16, review I-3): never create an FRig while another FRig has a TLS
// connection not yet destroyed by LWS; stop/join/destroy the earlier rig before creating another context.
// Each lws_create_context re-allocates this LWS copy's process-global
// openssl_websocket_private_data_index (ssl.c.o lws_context_init_ssl_library, stores at 0x1f4 and 0x23a), and the
// client verify callback dereferences what SSL_get_ex_data returns under it without a null check (openssl-client.c.o
// 0x1c8-0x1d6), so a handshake in flight across a context creation crashes.

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"
#include "VaRestJson.h"
#include "VaRestLwsProbeLws.h"

#include <atomic>

class FAutomationTestBase;
class FRunnableThread;

DECLARE_LOG_CATEGORY_EXTERN(LogVaRestLwsProbe, Log, All);

#if WITH_DEV_AUTOMATION_TESTS
namespace VaRest::LwsProbe
{
	/** Rig actions recorded next to LWS's callbacks: negative, so never an lws_callback_reasons value (0-75, 1000) */
	enum EPseudoReason : int32
	{
		ConnectCalling = -1,  // just before lws_client_connect_via_info
		ConnectReturned = -2, // Note: "wsi" or "null"
		CommandRan = -3,      // Note: the command's label, or "<label>: no wsi" when its connection was gone
		WriteDone = -4,       // Len: payload bytes; Note: "<label> ret=<lws_write> partial=<lws_partial_buffered>"
		CloseRequested = -5,  // Note: "code=<c>": lws_close_reason, then -1 from WRITEABLE
		HeadersAppended = -6, // Len: bytes written in APPEND_HANDSHAKE_HEADER; Note: "all" or "stopped: no space"
		Marker = -7,          // a hook's own note: Len and Note are the hook's
	};

	enum EEventFlags : uint32
	{
		FirstFragment = 1u << 0,       // lws_is_first_fragment, on CLIENT_RECEIVE
		FinalFragment = 1u << 1,       // lws_is_final_fragment
		BinaryFrame = 1u << 2,         // lws_frame_is_binary
		UserIsConnection = 1u << 3,    // the callback's user argument was this connection's userdata
		WsiUserIsConnection = 1u << 4, // lws_wsi_user(wsi) was this connection's userdata
	};

	/** One callback, or one rig action, in service-thread order */
	struct FEvent
	{
		int64 Sequence = 0;   // global append order, including pseudo events
		int32 WsiToken = 0;   // stable for one WSI lifetime; never an address
		bool bInputPresent = false; // CONNECTION_ERROR's input is non-null, even when Len is zero
		int32 CertDepth = -1;
		int32 CertError = -1;
		int32 Preverify = -1; // certificate callback's len is preverify_ok, not a byte count
		int32 Conn = 0;       // the rig's connection id; 0 when the callback belongs to no connection
		int32 Reason = 0;     // lws_callback_reasons or EPseudoReason
		double Time = 0.0;    // FPlatformTime::Seconds() when recorded
		int64 Len = 0;        // the callback's len, or the pseudo event's number
		uint32 Flags = 0;     // EEventFlags
		int64 Remaining = -1; // lws_remaining_packet_payload, on CLIENT_RECEIVE
		TArray<uint8> Bytes;  // a copy of in for reasons that carry bytes (at most 64 KiB each, 64 MiB per rig)
		FString Note;
	};

	/** Service thread (or stopped-context destruction): bounded input capture, no addresses logged. */
	void CaptureErrorInput(FEvent& OutEvent, const void* Input, size_t InputLength);

	/** Service thread, then game thread only after join. Keys never leave this object. */
	struct FTraceIdentity
	{
		struct FToken { int32 Id = 0; int32 Conn = 0; };
		TMap<const void*, FToken> Live;
		int32 Next = 1;
		FToken Observe(const void* Pointer, int32 ConnectionId, bool bDestroy)
		{
			if (!Pointer) { return {}; }
			FToken& Token = Live.FindOrAdd(Pointer);
			if (Token.Id == 0) { Token.Id = Next++; }
			if (ConnectionId != 0) { Token.Conn = ConnectionId; }
			const FToken Result = Token;
			if (bDestroy) { Live.Remove(Pointer); }
			return Result;
		}
	};

	/** The runner's test server: -VaRestTestServer=http://host:port and -VaRestTestServerTls=https://host:port */
	struct FServer
	{
		FString HttpBase;
		FString Host;
		int32 Port = 0;
		int32 TlsPort = 0; // 0: no TLS listener
	};

	/** AddError and false when the runner did not pass the server (or its TLS listener, when bNeedTls) */
	bool FindServer(FAutomationTestBase& Test, FServer& Out, bool bNeedTls = false);

	/** A fact's verdict: what the spec says (or inferred) against what LWS 3.0 did; Recorded: a measurement the spec does not claim */
	enum class EVerdict : uint8
	{
		Agrees,
		Differs,
		Recorded,
		Unverified,
	};

	inline EVerdict AgreesIf(bool bAgrees)
	{
		return bAgrees ? EVerdict::Agrees : EVerdict::Differs;
	}

	inline const TCHAR* YesNo(bool bValue)
	{
		return bValue ? TEXT("yes") : TEXT("no");
	}

	/** One probe result: a FACT line (JSON) in the log for the passport script, and an AddInfo in the test's report */
	void Fact(FAutomationTestBase& Test, const FString& Id, EVerdict Verdict, const FString& Observed, const FString& Spec, const FString& Evidence);

	/** Logged and flushed right before a call that may end the process: an id with PENDING and no FACT is "crashed" */
	void Pending(const FString& Id);

	/**
	 * A library outcome (opened, ended, echoed...), recorded as an AddInfo and never a failure: the facts carry it
	 * (WS W0 decision 3, review M-2 ruling). Returns bOutcome, so the caller can still branch on it.
	 */
	bool NoteOutcome(FAutomationTestBase& Test, const FString& What, bool bOutcome);

	/** Set by FVaRestTestsModule::StartupModule (VaRestTestsModule.cpp) */
	extern bool GWebSocketsLoadedAtModuleStartup;

	struct FConn;
	class FRig;

	/** A hook returns this to let the rig's default handling of the callback run */
	constexpr int32 KeepDefault = MIN_int32;

	/** Service thread, after the rig recorded the callback; any other return value is the callback's */
	using FHook = TFunction<int32(FConn& Conn, lws* Wsi, int32 Reason, void* User, void* In, size_t Len)>;

	/** What one connection does; set on the game thread before Connect, read on the service thread */
	struct FConnectSpec
	{
		int32 RouteId = 0;
		FString Path = TEXT("/ws/echo");   // request target, query included
		bool bTls = false;                  // LCCSCF_USE_SSL, to the server's TLS port unless Port says otherwise
		FString Address;                    // where TCP goes; empty: the server's host
		int32 Port = 0;                     // 0: the server's port (its TLS port when bTls)
		FString Host;                       // the Host header (and the name TLS checks); empty: "<Address>:<Port>"
		TOptional<FString> Origin;          // unset: no origin
		FString Protocols;                  // the offered subprotocols, as LWS takes them (comma separated); empty: none
		FString LocalProtocolName = TEXT("varest"); // local_protocol_name (libwebsockets.h:3455-3461); empty: NULL
		int32 SslFlags = 0;                 // LCCSCF_* added to LCCSCF_USE_SSL
		TArray<FString> AppendHeaders;      // "Name: value" lines, CRLF added, written in APPEND_HANDSHAKE_HEADER when they fit
		int32 PeerCloseReturn = 0;          // WS_PEER_INITIATED_CLOSE's return: 0 lets LWS echo (libwebsockets.h:1217-1225)
		FHook Hook;
	};

	/** One queued lws_write: LWS_PRE bytes of headroom, then the payload (libwebsockets.h:4846-4877) */
	struct FWrite
	{
		TArray<uint8> Buffer;
		int32 Protocol = 0; // lws_write_protocol, with LWS_WRITE_NO_FIN when it is not a message's last fragment
	};

	/** One connection's state; the service thread owns everything but Rig, Id and Spec */
	struct FConn
	{
		FRig* Rig = nullptr;
		int32 Id = 0;
		SSL* ObservedSsl = nullptr; // Opaque identity only; never dereferenced after retirement.
		lws* Wsi = nullptr;   // from connect_info.pwsi, cleared at WSI_DESTROY
		bool bDestroyed = false;
		FConnectSpec Spec;
		/** UTF-8 copies of the strings handed to lws_client_connect_via_info, alive as long as the connection */
		TArray<ANSICHAR> PathUtf8, AddressUtf8, HostUtf8, OriginUtf8, ProtocolsUtf8, LocalNameUtf8;
		TArray<FWrite> Outbox;
		int32 CloseCode = 0;
		TArray<uint8> CloseReason;
		/** Scratch for hooks */
		TMap<FName, int64> Counters;
	};

	enum class ETls : uint8
	{
		None,            // no TLS in the context
		LwsGlobalInit,   // LWS initialises OpenSSL and makes its own SSL_CTX (LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT)
		SslModule,       // the engine's order: the SSL module's SSL_CTX when InitializeSsl succeeds, else LwsGlobalInit (LwsWebSocketsManager.cpp:205-225)
		ProvidedContext, // FRigOptions::ProvidedSslContext as provided_client_ssl_ctx; the caller frees it after the rig
	};

struct FRouteSpec
{
    FString Name;
    FString ProxyAddress;
    uint32 ProxyPort = 0;
    bool bClientTls = true;
};
struct FRoute
{
    int32 Id = 0;
    FRouteSpec Spec;
    TArray<ANSICHAR> NameUtf8;
    TArray<ANSICHAR> ProxyUtf8;
    lws_vhost* Vhost = nullptr;
};
/** Any thread, pure: the fixed raw-rig allocation budget. */
inline bool CanAddRoute(int32 Existing) { return Existing >= 0 && Existing < 8; }

inline bool WithOwnedToken(const void* Candidate, TConstArrayView<const void*> Known, TFunctionRef<void()> Inspect)
{
    if (!Candidate || !Known.Contains(Candidate)) { return false; }
    Inspect();
    return true;
}

	struct FRigOptions
	{
		bool bExplicitRoutes = false;
		/** Service thread, synchronous before/after public route destruction; shared captures only. */
		TFunction<void(FRig&, bool)> OnRouteDestroyStage;
		/** Unset: null proxy address (LWS may use the environment); set: nonempty host:port. */
		TOptional<FString> ProxyAddress;
		/** Explicit event cap; every omitted event increments DroppedEventCount(). */
		int32 MaxRecordedEvents = MAX_int32;
		/** lws_service's wait; the production shape is a long wait woken by lws_cancel_service (WebSocket spec 2.1) */
		int32 ServiceTimeoutMs = 10000;
		/** The engine's options (LwsWebSocketsManager.cpp:184) plus JUST_USE_RAW_ORIGIN (WebSocket spec 2.1) */
		uint32 ContextOptions = LWS_SERVER_OPTION_PEER_CERT_NOT_REQUIRED | LWS_SERVER_OPTION_DISABLE_OS_CA_CERTS | LWS_SERVER_OPTION_JUST_USE_RAW_ORIGIN;
		ETls Tls = ETls::None;
		SSL_CTX* ProvidedSslContext = nullptr;
		/** lws_context_creation_info::timeout_secs (libwebsockets.h:2791-2795); 0: LWS's default */
		uint32 TimeoutSecs = 0;
		/** The one protocol's rx_buffer_size; 64 KiB as the engine's (LwsWebSocketsManager.cpp:166) */
		uint32 RxBufferSize = 65536;
		/** true: commands run inside LWS_CALLBACK_EVENT_WAIT_CANCELLED (WebSocket spec 2.1); false: between lws_service calls */
		bool bCommandsInsideCallback = true;
		/** Service thread (or inside lws_create_context): LOAD_EXTRA_CLIENT_VERIFY_CERTS, whose user is the SSL_CTX* (libwebsockets.h:961-967) */
		TFunction<void(SSL_CTX* Ctx)> OnLoadExtraClientVerifyCerts;
		/** Service thread: PERFORM_SERVER_CERT_VERIFICATION; its return is the callback's (0 keeps OpenSSL's verdict, libwebsockets.h:1015-1044) */
		TFunction<int32(FConn& Conn, X509_STORE_CTX* Store, int64 PreverifyOk)> OnVerifyServerCert;
	};

	class FRig final : public FRunnable
	{
	public:
		/** Game thread: creates the context and starts the service thread; IsReady() says whether both worked */
		explicit FRig(const FServer& InServer, FRigOptions InOptions = FRigOptions());
		virtual ~FRig() override;
		FRig(const FRig&) = delete;
		FRig& operator=(const FRig&) = delete;

		bool IsReady() const { return Context != nullptr && Thread != nullptr; }
		bool UsedSslModuleContext() const { return bSslModuleContext; }
		const FServer& GetServer() const { return Server; }

		/** Game thread: stops the service thread, then destroys the context (open connections end inside); the events stay readable */
		void Shutdown();
/** Game thread: allocate a monotone id and enqueue creation; 0 refuses the eight-route cap. */
int32 AddRoute(FRouteSpec InSpec);
/** Game thread: enqueue public vhost destruction; retained route strings survive context destruction. */
void DestroyRoute(int32 InRouteId);
/** Game thread: queue a service-thread SSL/WSI identity witness, never exposes addresses. */
void RecordSslIdentity(int32 ConnId, const TCHAR* Label);

        /** Game thread: queue the connection whose live identity brackets route operations. */
        void WatchConnection(int32 ConnId);
        /** Service thread synchronously, or stopped-context destruction: observed live watched identity. */
        FVaRestJson WatchedSslIdentity();
        /** Service thread synchronously, only after matching the live owned WSI. */
        FVaRestJson SslIdentity(FConn& Connection);


		/** Game thread: a new connection, connected on the service thread; returns its id (> 0) */
		int32 Connect(FConnectSpec InSpec);
		/** Game thread: Command runs on the service thread, recorded as CommandRan with Label */
		void Exec(const TCHAR* Label, TFunction<void(FRig&)> Command);
		/** ... with the connection; recorded as "<Label>: no wsi" (and not run) when the connection is gone */
		void ExecOn(int32 ConnId, const TCHAR* Label, TFunction<void(FConn&)> Command);
		/**
		 * Queues one lws_write; WRITEABLE writes one per callback, as WebSocket spec 2.4 plans. Also before the open: LWS
		 * drops a writable request made during the handshake, so CLIENT_ESTABLISHED asks again when anything is queued
		 */
		void Write(int32 ConnId, int32 Protocol, TArray<uint8> Payload);
		void WriteText(int32 ConnId, const FString& Text, int32 Protocol = LWS_WRITE_TEXT);
		/**
		 * The engine's close: lws_close_reason, then -1 from WRITEABLE once the outbox is empty (LwsWebSocket.cpp:515-530);
		 * queued before the open, it goes once the connection opens (as Write)
		 */
		void Close(int32 ConnId, int32 Code, const FString& Reason);

		/** Game thread: pumps (game-thread tasks and the core ticker) until Done holds over the events, or Seconds pass */
		bool WaitFor(TFunctionRef<bool(const TArray<FEvent>&)> Done, double Seconds = 15.0);
		bool WaitReason(int32 ConnId, int32 Reason, double Seconds = 15.0);
		/** WSI_DESTROY, or a connect that returned null */
		bool WaitEnded(int32 ConnId, double Seconds = 15.0);
		/** A copy of the events; 0: all of them */
		TArray<FEvent> Events(int32 ConnId = 0) const;
		/** Any thread: number omitted by the explicit event cap; nonzero invalidates evidence. */
		int32 DroppedEventCount() const;
		/** The first event of ConnId with Reason (and a Note starting with NotePrefix) */
		bool Find(int32 ConnId, int32 Reason, FEvent& Out, const TCHAR* NotePrefix = nullptr) const;

		/** Any thread: a pseudo event */
		void Record(int32 ConnId, int32 Reason, int64 Len, const FString& Note);
		/** Service thread, inside a callback of Wsi: one lws_write now, recorded as WriteDone with Label */
		int32 WriteNow(FConn& Conn, lws* Wsi, int32 Protocol, TConstArrayView<uint8> Payload, const TCHAR* Label);

		/** The protocol's callback (lws_protocols::callback) */
		static int Callback(lws* Wsi, lws_callback_reasons Reason, void* User, void* In, size_t Len);

	private:
        TArray<TUniquePtr<FRoute>> Routes;
        int32 NextRouteId = 1;
        int32 RequestedRoutes = 0; // Game-thread admission, independent of service-thread storage.
        int32 WatchedConnectionId = 0;
        FTraceIdentity SslTokens;
        void AddRouteNow(int32 InRouteId, FRouteSpec InSpec);
        FRoute* FindRoute(int32 InRouteId);
        int32 HandleRoute(lws* InWsi, int32 Reason, void* User, void* In, size_t Len);
		virtual uint32 Run() override;
		virtual void Stop() override;
		void Enqueue(TFunction<void()> Command);
		void DrainCommands();
		int32 Handle(lws* Wsi, int32 Reason, void* User, void* In, size_t Len);
		void ConnectNow(FConn& Conn);
		FConn* FindConn(lws* Wsi, void* User);
		int32 AppendHeaders(FConn& Conn, void* In, size_t Len);
		int32 WriteNext(FConn& Conn, lws* Wsi);
		void Append(FEvent&& Event);
		void CopyBytes(FEvent& Event, const void* In, size_t Len);

		FServer Server;
		FRigOptions Options;
		TArray<ANSICHAR> ProxyUtf8;
		FTraceIdentity TraceIdentity;
		int32 ConnectingId = 0;
		int64 NextSequence = 1;
		int32 DroppedEvents = 0;
		lws_context* Context = nullptr;
		lws_protocols Protocols[2] = {};
		FRunnableThread* Thread = nullptr;
		std::atomic<bool> bStopping{ false };
		TQueue<TFunction<void()>, EQueueMode::Mpsc> Commands;
		mutable FCriticalSection ConnsLock;
		TArray<TUniquePtr<FConn>> Conns;
		mutable FCriticalSection EventsLock;
		TArray<FEvent> Recorded;
		int64 CopiedBytes = 0;
		SSL_CTX* SslModuleContext = nullptr;
		bool bSslInitialized = false;
		bool bSslModuleContext = false;
		std::atomic<int32> NextId{ 1 };
	};

	// ---- helpers (game thread)

	FString NewKey();
	FString Utf8Text(const void* Data, size_t Num);
	FString EventText(const FEvent& Event);
	TArray<uint8> Utf8Bytes(const FString& Source);
	FString HexOf(TConstArrayView<uint8> Bytes);
	FString ReasonName(int32 Reason);
	/** Reason names joined with " > ", runs collapsed ("RECEIVE x4"); rig bookkeeping left out */
	FString CallSequence(const TArray<FEvent>& Events);
	TArray<FEvent> OfReason(const TArray<FEvent>& Events, int32 Reason);
	bool Opened(const TArray<FEvent>& Events);
	/** CONNECTION_ERROR's string, or "(no CONNECTION_ERROR: <sequence>)" */
	FString ErrorOf(const TArray<FEvent>& Events);
	/** What lws_client_connect_via_info returned: "wsi", "null" or "(not called)" */
	FString ConnectResult(const TArray<FEvent>& Events);
	/** Connects, waits for the open or the end, closes 1000 if it opened, waits for the end; a connection that never ends is noted (NoteOutcome), and false returned */
	bool Visit(FAutomationTestBase& Test, FRig& Rig, FConnectSpec Spec, TArray<FEvent>& Out, double Seconds = 20.0);
	/** Until the connection's CLIENT_RECEIVE lengths add up to Bytes */
	bool WaitBytes(FRig& Rig, int32 ConnId, int64 Bytes, double Seconds = 15.0);
	/** GET /ws/journal over HTTP (FVaRestClient); an invalid handle and an AddError on failure */
	FVaRestJson FetchJournal(FAutomationTestBase& Test, const FServer& Site, const FString& Key, int32 Connections = 1, int32 WaitMs = 5000);
	/** The journal's events of one kind ("ev"), of one connection (1-based) or all (0) */
	TArray<FVaRestJson> JournalEvents(const FVaRestJson& Journal, const TCHAR* Kind, int32 ConnIndex = 0);
	/** A request header the server received (name compared case-insensitively), or "(absent)" */
	FString JournalHeader(const FVaRestJson& Journal, const TCHAR* Name, int32 ConnIndex = 1);
	/** An event's "b64" payload */
	TArray<uint8> JournalBytes(const FVaRestJson& Event);
	bool FetchBytes(FAutomationTestBase& Test, const FServer& Site, const FString& Path, TArray<uint8>& Out);
	/** Adds every PEM certificate in Pem to the context's trust store; the number added */
	int32 AddPemToStore(SSL_CTX* Ctx, TConstArrayView<uint8> Pem);
	/** Percent-encodes a query value */
	FString UrlValue(const FString& Value);
	/** LWS_WRITE_TEXT or LWS_WRITE_BINARY, without the flags: the first frame of a message */
	bool IsMessageStart(int32 Protocol);
}
#endif // WITH_DEV_AUTOMATION_TESTS
