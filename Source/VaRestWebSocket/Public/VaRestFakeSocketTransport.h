// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestSocketTransport.h"

#include <atomic>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaRest::WebSocket
{
	class FSocketLink;
}

class FVaRestFakeSocketConnection;

/**
 * One attempt at FVaRestFakeSocketTransport (WebSocket spec 16.1): what the consumer asked -- the request, its sends, its
 * close, what it consumed -- and the server's side, which the test scripts on the game thread at any time: from the test
 * body, from FVaRestFakeSocketTransport::OnConnect, or from inside the consumer's own handlers. The events go into the same
 * ordered queue the real transport fills. A script call notifies at once; one made inside a consumer's call into the
 * connection (Connect, Send, Close) notifies from a later game-thread task, so the fake never calls back inside the
 * consumer's call (E7). It does not check the request (the socket layer's own tests do).
 *
 * Dropping the consumer's last handle is the release (plan decision 5, spec 1.4): WasReleased is true at once and every
 * event from then on is discarded, never notified; then, on the game thread (at once, or from a game-thread task when the
 * handle was dropped elsewhere), an attempt not yet ended or closed is closed as the real handle's release closes it --
 * 1001 "released" when open (answered at once when bEchoClose), cancelled when pending.
 */
class VARESTWEBSOCKET_API FVaRestFakeSocketAttempt final
{
public:
	FVaRestFakeSocketAttempt(int32 InIndex, FVaRestSocketConnectRequest&& InRequest, TFunction<void()> InOnEvents);

	int32 GetIndex() const { return Index; }
	const FVaRestSocketConnectRequest& GetRequest() const { return Request; }

	// ---- the server's side (game thread); after the terminal event each one is dropped, with an Error log line
	FVaRestFakeSocketAttempt& Open(const FString& InProtocol = FString(), double ConnectSeconds = 0.0);
	FVaRestFakeSocketAttempt& Message(EVaRestSocketFrameKind InKind, TConstArrayView<uint8> InBytes);
	FVaRestFakeSocketAttempt& Text(FStringView InText);
	FVaRestFakeSocketAttempt& Pong(double RttSeconds);
	/** Closed: By Server, clean */
	FVaRestFakeSocketAttempt& ServerClose(int32 Code, const FString& Reason = FString());
	/** Closed: 1006, By Transport, not clean */
	FVaRestFakeSocketAttempt& Drop();
	/** Error before the open: By Timeout for ConnectTimeout, else Transport */
	FVaRestFakeSocketAttempt& Fail(EVaRestSocketFailure Failure, int32 HttpStatus = 0, const FString& Detail = FString());
	/** Any terminal event (Type Closed or Error) */
	FVaRestFakeSocketAttempt& End(EVaRestSocketEventType Type, const FVaRestSocketEnd& InEnd);
	/** Bytes of the consumer's sends (all, by default) reach "the socket": the buffered amount falls, Drained per spec 2.4 */
	FVaRestFakeSocketAttempt& Flush(int64 Bytes = MAX_int64);

	/** true (default): the consumer's Close on an open attempt is answered at once -- Closed, By Client, clean, its code and reason. false: the test ends it */
	bool bEchoClose = true;

	// ---- what the consumer did
	const TArray<FVaRestSocketFrame>& GetSent() const { return Sent; }
	bool WasCloseRequested() const { return bCloseRequested; }
	/** The close as the real transport sends it: the code sanitised, the reason cut (spec 3.5) */
	int32 GetCloseCode() const { return CloseCode; }
	const FString& GetCloseReason() const { return CloseReason; }
	/**
	 * Any thread: release and Consumed may come from the consumer's thread (W3's pipe), so both are atomic (review conc M-4).
	 * The flag is acquire (a reader that sees it sees what came before the release); the count is relaxed, a count.
	 */
	bool WasReleased() const { return bReleased.load(std::memory_order_acquire); }
	int64 GetConsumed() const { return ConsumedBytes.load(std::memory_order_relaxed); }
	bool IsOpen() const { return bOpen && !HasEnded(); }
	bool HasEnded() const;

private:
	friend class FVaRestFakeSocketTransport;
	friend class FVaRestFakeSocketConnection;

	void Push(FVaRestSocketEvent&& Event);
	bool OnSend(FVaRestSocketFrame&& Frame);
	void OnClose(int32 Code, FStringView Reason);
	void OnConsumed(int64 Bytes);
	/** Any thread: the handle is gone; from here the link discards */
	void OnReleased();
	/** Game thread, after OnReleased: the release's close or cancel (plan decision 5) */
	void CloseReleased();

	const int32 Index;
	FVaRestSocketConnectRequest Request;
	TSharedRef<VaRest::WebSocket::FSocketLink, ESPMode::ThreadSafe> Link;
	/** > 0 while the consumer is inside one of its calls into the connection (or inside Connect) */
	int32 ConsumerDepth = 0;
	bool bOpen = false;
	bool bCloseRequested = false;
	int32 CloseCode = 0;
	FString CloseReason;
	std::atomic<bool> bReleased{ false };
	std::atomic<int64> ConsumedBytes{ 0 };
	TArray<FVaRestSocketFrame> Sent;
};

/** The fake transport of WebSocket spec 16.1, for the socket layer's tests (W2-W5). Game thread. Not for games. */
class VARESTWEBSOCKET_API FVaRestFakeSocketTransport final : public IVaRestSocketTransport
{
public:
	virtual TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connect(FVaRestSocketConnectRequest&& Request, TFunction<void()> OnEvents = nullptr) override;

	/** Runs for each attempt as Connect starts it: script it there (Open, Fail, ...) or later through GetAttempt */
	TFunction<void(FVaRestFakeSocketAttempt&)> OnConnect;

	int32 NumAttempts() const { return Attempts.Num(); }
	FVaRestFakeSocketAttempt& GetAttempt(int32 InIndex) const { return *Attempts[InIndex]; }
	FVaRestFakeSocketAttempt& GetLast() const { return *Attempts.Last(); }

private:
	TArray<TSharedRef<FVaRestFakeSocketAttempt, ESPMode::ThreadSafe>> Attempts;
};

#endif // WITH_DEV_AUTOMATION_TESTS
