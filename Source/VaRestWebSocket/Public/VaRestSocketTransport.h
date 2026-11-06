// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "VaRestSocketTypes.h"
#include <atomic>

/** Native service counters shared across attempts; no UObject, client or callback crosses the service boundary. */
struct FVaRestSocketServiceStats
{
	std::atomic<int64> AutoRepliesSent{0};
	std::atomic<double> LongestAutoReplyDelay{0.0};
	void RecordAutoReply(double Delay)
	{
		AutoRepliesSent.fetch_add(1, std::memory_order_relaxed);
		if (!FMath::IsFinite(Delay) || Delay < 0.0) return;
		double Previous = LongestAutoReplyDelay.load(std::memory_order_relaxed);
		while (Delay > Previous && !LongestAutoReplyDelay.compare_exchange_weak(Previous, Delay, std::memory_order_relaxed)) {}
	}
};

namespace VaRest::WebSocket
{
	/** The largest ConnectTimeout VaRest accepts: the LWS context's timeout_secs, the backstop of every handshake stage (spec 2.2) */
	inline constexpr double MaxConnectTimeoutSeconds = 60.0;
}

/** Native immutable exact-text rule. Reply data is written only at a data-message boundary. */
struct FVaRestSocketAutoReply
{
	TArray<uint8> Match, Reply;
};

/**
 * One connection attempt (WebSocket spec 2.2; D3: one LWS connection per attempt, its headers fresh each time). The socket
 * layer (W2) fills it from its configuration on the game thread; W1b adds the ping, flow-control and auto-reply fields.
 */
struct FVaRestSocketConnectRequest
{
	/** ws://, wss://, or (not in Shipping) wss+insecure://. http(s):// is the socket layer's sugar (spec 3.2), not the transport's */
	FString Url;
	/** Offered subprotocols: RFC 7230 tokens, each once. The connection binds to VaRest's handler whatever the server selects (T3, T17) */
	TArray<FString> Protocols;
	/** Appended to the upgrade (spec 2.2, D23): names RFC 7230 tokens, values VCHAR, space or tab; never Host, Upgrade, Connection, Origin or Sec-WebSocket-* */
	TArray<TPair<FString, FString>> Headers;
	/** Unset: derived from the URL (https:// for wss, http:// for ws, the host and a non-default port; D29) and sent as given (T4). Set: as given. Set to "": no Origin */
	TOptional<FString> Origin;
	/** Not in Shipping: what wss+insecure:// does for a wss:// URL -- a self-signed chain allowed, the host name not checked (spec 2.2, 15) */
	bool bAllowSelfSignedTls = false;
	/** Seconds from the connect to the open, measured on the service thread (spec 2.2, D32); above MaxConnectTimeoutSeconds it is clamped, with a Warning */
	double ConnectTimeout = 10.0;
	/** Seconds a close VaRest started may take before the connection is killed; LWS's own close-ack wait is a fixed 5 s (T20), so a longer value acts as about 5 s */
	double CloseTimeout = 5.0;
	/** An inbound message over this closes the connection with 1009 (spec 2.3, 15) */
	int64 MaxMessageBytes = 1024 * 1024;
	int64 MaxPendingInboundBytes = 16 * 1024 * 1024;
	/** Compatibility field for consumers reporting logical frames. Private queue-slot pressure is independent of ACKs. */
	bool bExplicitMessageConsumption = false;
	double PingInterval = 20.0, PingTimeout = 10.0;
	TArray<FVaRestSocketAutoReply> AutoReplies;
	TSharedPtr<FVaRestSocketServiceStats, ESPMode::ThreadSafe> ServiceStats;
	/** Once the buffered amount has reached this, its next return to zero is reported as Drained (spec 2.4, 6.3) */
	int64 SendHighWater = 256 * 1024;
	/** Log lines name the connection by this (W2: the socket's name and attempt); empty: "ws#<id>" */
	FString LogName;
};

/**
 * One attempt's connection (WebSocket spec 1.3). Its events arrive in one ordered queue (spec 2.1, 7.1): Open first when it
 * opens, then Message and Drained (and W1b's Pong) in wire order, then exactly one terminal event -- Error when it never
 * opened, Closed when it did (plan decision 2). Nothing comes ahead of the Open: a send made before it may reach the
 * high-water mark, and its Drained follows the Open (plan decision 18). Destroying the last reference releases it (any
 * thread): an open connection is closed with 1001 "released" (spec 1.4), a pending one is cancelled, and its remaining
 * events are discarded.
 */
class IVaRestSocketConnection
{
public:
	virtual ~IVaRestSocketConnection() = default;

	/** The consumer's thread (one at a time): appends at most the entry snapshot of queued events in FIFO order.
	 * Concurrently published events can remain for another pull; returns actual additions. Dequeue releases private
	 * queue-slot credit even for empty messages; payload bytes stay charged until Consumed. */
	virtual int32 PopEvents(TArray<FVaRestSocketEvent>& Out) = 0;

	/**
	 * Game thread: queues a message (spec 2.4); false when the frame is invalid, the connection has ended, or Close was called
	 * on it (review conc M-6: the refusal is synchronous, never a silent drop). A send before the open goes out after it.
	 */
	virtual bool Send(FVaRestSocketFrame&& Frame) = 0;

	/**
	 * Game thread: our close (spec 3.5: a code outside 1000-1003, 1007-1014, 3000-4999 becomes 1000 and the reason is cut to
	 * 123 bytes, each with a Warning); it goes after the data already queued. Before the open it cancels the attempt.
	 */
	virtual void Close(int32 Code, FStringView Reason) = 0;

	/** The consumer's thread: Bytes of Message events it has delivered (spec 2.3: flow control counts what it has not) */
	virtual void Consumed(int64 Bytes) = 0;
	/** Exact logical-frame retirement for consumers holding fragmented packets, including empty messages.
	 * This is independent of transport queue-slot credit, which retires on PopEvents. */
	virtual void ConsumedMessages(int64 Bytes, int32 Messages) { Consumed(Bytes); }
	/** Service-thread controls. False means unsupported or refused; callers must handle admission explicitly. */
	virtual bool SetIdleDeadline(double Seconds) { return false; }
	virtual bool Abort(FStringView Reason, EVaRestSocketCloseBy By = EVaRestSocketCloseBy::Transport,
		EVaRestSocketFailure Failure = EVaRestSocketFailure::None) { return false; }
	virtual bool IsReadPaused() const { return false; }

	/** Any thread: bytes Send accepted that the socket has not taken yet, exact to within one 64 KiB fragment (spec 2.4, 6.3; plan decision 7) */
	virtual int64 GetBufferedAmount() const = 0;

	/** Any thread: bytes of queued Message events not yet Consumed */
	virtual int64 GetPendingInboundBytes() const = 0;

	/** Any thread: true once the terminal event is queued */
	virtual bool HasEnded() const = 0;
};

/** The seam (WebSocket spec 1.3): VaRest's libwebsockets transport ships (spec 2); tests use FVaRestFakeSocketTransport */
class IVaRestSocketTransport
{
public:
	virtual ~IVaRestSocketTransport() = default;

	/**
	 * Game thread: starts one attempt. Never calls back inside: a refusal -- a bad URL, the allowlist, a header -- is an Error
	 * event queued like any other (E7). OnEvents, when set, runs after events are queued, on the thread that queued them: the
	 * service thread, or the game thread -- synchronously inside the transport's Shutdown (each connection's Shutdown end),
	 * and from a game-thread task for a refusal or a transport that could not start. It only schedules work and never calls
	 * the connection's game-thread functions. It is destroyed on whichever thread drops the connection's state last (often the
	 * service thread, when LWS destroys the connection), so what it captures must be weak or thread-safe: a strong reference
	 * to the connection's owner makes a cycle (connection -> OnEvents -> owner -> connection) that lasts until the server ends
	 * the connection (review conc M-5).
	 */
	virtual TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connect(FVaRestSocketConnectRequest&& Request, TFunction<void()> OnEvents = nullptr) = 0;
};

namespace VaRest::WebSocket
{
	/**
	 * The process's libwebsockets transport (spec 2.1: one context per process; plan decision 8), made at first use and
	 * shut down at engine exit; a connect after that ends with Shutdown. Any thread; its Connect is game thread.
	 */
	VARESTWEBSOCKET_API TSharedRef<IVaRestSocketTransport, ESPMode::ThreadSafe> GetDefaultTransport();
}
