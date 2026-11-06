// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestSocket.h"

namespace VaRest::WebSocket { struct FSocketRequestControl; struct FSocketJsonRequestOptions; struct FSocketData; }

/** Weak native request identity. Cancel and IsActive may be used on any thread; no callbacks or GTK state are retained. */
class VARESTWEBSOCKET_API FVaRestSocketRequestHandle
{
public:
	FVaRestSocketRequestHandle() = default;
	FString GetId() const { return Id; }
	bool IsActive() const;
	void Cancel() const;
private:
	friend struct VaRest::WebSocket::FSocketData;
	TWeakPtr<VaRest::WebSocket::FSocketRequestControl, ESPMode::ThreadSafe> Control;
	FString Id;
};

/** GTK-only, move-only JSON request builder. Configure, Send and destroy on GT.
 * Timeout starts at Send, including readiness/backlog waits; zero disables it.
 * An empty socket or refused admission calls OnError synchronously on GT; admitted results use the socket tick. */
class VARESTWEBSOCKET_API FVaRestSocketJsonRequest
{
public:
	FVaRestSocketJsonRequest(FVaRestSocketJsonRequest&& Other);
	FVaRestSocketJsonRequest& operator=(FVaRestSocketJsonRequest&& Other);
	~FVaRestSocketJsonRequest();
	FVaRestSocketJsonRequest(const FVaRestSocketJsonRequest&) = delete;
	FVaRestSocketJsonRequest& operator=(const FVaRestSocketJsonRequest&) = delete;
	FVaRestSocketJsonRequest& Timeout(double Seconds);
	FVaRestSocketJsonRequest& Owner(const UObject* Owner);
	FVaRestSocketJsonRequest& OnSuccess(TFunction<void(const FVaRestJson&)> Handler);
	FVaRestSocketJsonRequest& OnError(TFunction<void(const FVaRestError&)> Handler);
	FVaRestSocketJsonRequest& InChannel(FString Channel);
	/** Protocol errors are classified before the terminal request trace and user callback. */
	FVaRestSocketJsonRequest& TransformReply(TUniqueFunction<FVaRestError(const FVaRestSocketMessage&, FVaRestJson&)>&& Transform);
	FVaRestSocketJsonRequest& OnReply(TFunction<void(const FVaRestSocketMessage&, const FVaRestJson&)> Handler);
	/** Protocol-owned cleanup, including silent owner/scope retirement. Never dispatch application callbacks here. */
	FVaRestSocketJsonRequest& OnRetired(TUniqueFunction<void()>&& Cleanup);
	[[nodiscard]] FVaRestSocketRequestHandle Send();
private:
	friend class FVaRestSocket;
	friend class FVaRestSocketChannel;
	template <typename T> friend class TVaRestSocketRequest;
	void SetChannelOnGT(FString Channel);
	void SetMessageSuccessOnGT(TFunction<void(const FVaRestSocketMessage&)> Handler);
	FVaRestSocketJsonRequest(FVaRestSocket Socket, FStringView Route, FVaRestJson Payload);
	FVaRestSocket Socket;
	TUniquePtr<VaRest::WebSocket::FSocketJsonRequestOptions> Options;
};

#include "VaRestSocketTypedRequest.h"
