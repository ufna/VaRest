// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestLwsService.h"
#include "VaRestSocketLink.h"
#include "VaRestSocketTarget.h"
#include "VaRestSocketTransport.h"

namespace VaRest::WebSocket::Lws
{
	enum class EConnState : uint8
	{
		/** Made on the game thread; Start has not run */
		Waiting,
		/** lws_client_connect_via_info was called; no open yet */
		Connecting,
		Open,
		/** The terminal event is queued; LWS may still call back until WSI_DESTROY */
		Ended
	};

	/**
	 * One connection (WebSocket spec 2.1-2.5): libwebsockets' userdata for it, owned by the FService from Adopt on. Made and
	 * prepared on the game thread by FTransport::Connect. From Adopt on every member is the service's: its functions run on
	 * the service thread, or on the game thread once that thread is joined -- inside Shutdown's drain of the queued commands,
	 * EndForShutdown, and lws_context_destroy, which delivers CLIENT_CLOSED and WSI_DESTROY -- never on both at once (the
	 * join orders them). Nothing here assumes which of the two it is on (Task 6 review M-2).
	 */
	class FConn final : public IServiceConnection
	{
	public:
		/** Game thread: InLink already carries the connection's label (MakeLabel) */
		FConn(uint64 InId, TSharedRef<FSocketLink, ESPMode::ThreadSafe> InLink, FVaRestSocketConnectRequest&& InRequest);

		/** Any thread: the last owner drops it -- a wss attempt still counted leaves TlsHandshakesInFlight() (re-review N-2: an Adopt command dropped unrun) */
		virtual ~FConn() override;

		/** Any thread: the name log lines give the connection -- the request's LogName, else "ws#<id>" (Task 3 review M-1) */
		static FString MakeLabel(uint64 InId, const FVaRestSocketConnectRequest& InRequest);

		/** Game thread, inside Connect: checks and encodes the request (spec 2.2); a failure is reported by Start, from the service thread (E7) */
		void Prepare(const FServiceOptions& Options);
		/** Any thread: the link's label, fixed at construction */
		const FString& GetLabel() const { return Link->GetLabel(); }
		/** Game thread, inside Connect: a wss target that Prepare took */
		bool NeedsTlsCount() const { return Target.bTls && PendingFailure == EVaRestSocketFailure::None; }
		/** Game thread, inside Connect, before the Adopt command is queued: raises TlsHandshakesInFlight() for this attempt (re-review N-2) */
		void MarkTlsCounted();

		// ---- IServiceConnection (the service's thread: see the class comment)
		virtual uint64 GetId() const override { return Id; }
		virtual void Start(FService& Service) override;
		virtual int32 OnCallback(FService& Service, lws* InWsi, int32 Reason, void* User, void* In, size_t Len) override;
		virtual void CheckDeadlines(FService& Service, double Now) override;
		virtual double NextDeadline() const override;
		virtual bool WantsShortWait() const override;
		virtual bool IsDestroyed() const override { return bDestroyed; }
		virtual void EndForShutdown() override;

		// ---- commands from the handle (the service's thread)
		/**
		 * BufferedAfterSend: the buffered amount FHandle::Send's own add produced, which the app saw; the high-water mark is
		 * decided on it, never on a re-read here (Task 3 review I-1, FSocketLink::NoteBuffered)
		 */
		void Send(FService& Service, FVaRestSocketFrame&& Frame, int64 BufferedAfterSend);
		/** Never gated on FService::IsShuttingDown: Shutdown's flush waits for the closes asked for before it (Task 6 review M-7) */
		void Close(FService& Service, int32 Code, TArray<uint8>&& Reason, EVaRestSocketCloseBy By, EVaRestSocketFailure Failure);
		void Release(FService& Service);

		/** The service's thread: the connection with InId, or null; every connection a transport adopts is an FConn */
		static FConn* Find(FService& Service, uint64 InId);

	private:
		int32 OnAppendHeaders(void* In, size_t Len);
		int32 OnFilterPreEstablish(lws* InWsi);
		int32 OnEstablished(lws* InWsi);
		int32 OnWriteable(lws* InWsi);
		/** PEER_INITIATED_CLOSE: the peer's close read by spec 2.5's rules (ParsePeerClose); 0, so LWS echoes it */
		int32 OnPeerClose(const void* In, size_t Len);
		/** PERFORM_SERVER_CERT_VERIFICATION: 0 accepts, 1 refuses (libwebsockets.h:1015-1044) */
		int32 OnVerifyServerCertificate(FService& Service, X509_STORE_CTX* Store, int32 PreverifyOk);
		void OnConnectionError(const void* In, size_t Len);
		/** CLIENT_CLOSED, CLOSED_CLIENT_HTTP, and WSI_DESTROY for what did not end before it */
		void OnEnded();
		void OnDestroyed(FService& Service);
		/** lws_set_timeout(LWS_TO_KILL_ASYNC) (T14): the end is reported by the caller, LWS tears TCP down later (plan decision 3) */
		void Kill(FService& Service);
		void ReleaseHeld();
		/** The outbox's bytes leave the buffered amount, with no Drained (an end) */
		void DropOutbox();
		void EndPreOpen(EVaRestSocketFailure Failure, EVaRestSocketCloseBy By, int32 HttpStatus, FString Detail);
		void EndOpen(int32 Code, FString Reason, bool bClean, EVaRestSocketCloseBy By, EVaRestSocketFailure Failure, FString Detail = FString());
		/** Once: this wss attempt leaves TlsHandshakesInFlight() (review conc I-3) */
		void UncountTls();
		/** Once: this connection's close leaves FServiceCounters::UnsentCloses (review conc M-3) */
		void UncountClose();

		const uint64 Id;
		const TSharedRef<FSocketLink, ESPMode::ThreadSafe> Link;
		FVaRestSocketConnectRequest Request;
		FSocketTarget Target;
		/** A failure VaRest decided -- in Prepare, or during the handshake -- that explains the end better than libwebsockets' string */
		EVaRestSocketFailure PendingFailure = EVaRestSocketFailure::None;
		FString PendingDetail;
		/**
		 * UTF-8, NUL-terminated copies of the strings lws_client_connect_via_info takes (libwebsockets.h:3402-3418), kept for
		 * the connection's life: the header does not say that LWS copies them
		 */
		TArray<ANSICHAR> AddressUtf8;
		TArray<ANSICHAR> PathUtf8;
		TArray<ANSICHAR> HostUtf8;
		TArray<ANSICHAR> OriginUtf8;
		TArray<ANSICHAR> ProtocolsUtf8;
		lws* Wsi = nullptr;
		/** Set by Start: the service that owns this connection */
		FService* Owner = nullptr;
		EConnState State = EConnState::Waiting;
		bool bDestroyed = false;
		/** In TlsHandshakesInFlight(): a wss attempt from its connect until CLIENT_ESTABLISHED or WSI_DESTROY (review conc I-3) */
		bool bTlsCounted = false;
		double StartedAt = 0.0;
		int32 AppendCalls = 0;
		FString SelectedProtocol;
		bool bKillIssued = false;
		double KillAt = 0.0;
		/** When the pending kill was last armed: Kill sets it, CheckDeadlines' re-arm moves it (re-review N-1) */
		double KillArmedAt = 0.0;
		// ---- the outbox (plan decisions 6, 7, 18)
		TArray<FVaRestSocketFrame> Outbox;
		/** Bytes of a write LWS still holds (lws_partial_buffered); released once it says the write drained */
		int64 HeldBytes = 0;
		/** Fragments of Outbox[0] already written (plan decision 6); 0 at a message boundary */
		int32 NextFragment = 0;
		// ---- our close (spec 2.5)
		bool bCloseQueued = false;
		bool bCloseSent = false;
		/** In FServiceCounters::UnsentCloses: asked for while open, not written yet (review conc M-3) */
		bool bCloseCounted = false;
		/** When OnWriteable wrote our close frame: an end by LWS's own 5 s close-ack timer after it is not clean (review conc I-5, Task 9) */
		double CloseSentAt = 0.0;
		int32 CloseCode = 0;
		TArray<uint8> CloseReason;
		EVaRestSocketCloseBy CloseBy = EVaRestSocketCloseBy::Client;
		EVaRestSocketFailure CloseFailure = EVaRestSocketFailure::None;
		/** FPlatformTime::Seconds() past which our close is cut: CloseTimeout from the Close command (plan decision 17) */
		double CloseDeadline = 0.0;
		// ---- the peer's close (spec 2.5)
		bool bPeerClosed = false;
		VaRest::WebSocket::FPeerClose Peer;
	};
}
