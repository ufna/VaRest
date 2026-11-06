// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestSocket.h"

/** Logical channel of one physical socket. Handles share socket lifetime; configuration and observers are GTK-only. */
class VARESTWEBSOCKET_API FVaRestSocketChannel
{
public:
	FVaRestSocketChannel() = default;
	bool IsValid() const { return Socket.IsValid(); }
	FString GetName() const;
	bool IsConnected() const;
	bool IsReady() const;
	FVaRestSocketWeak SocketAsWeak() const { return Socket.AsWeak(); }
	FVaRestSocketChannel ReadyWhen(FString Route);
	FVaRestSocketChannel OnConnected(const UObject* Owner, TFunction<void(const FVaRestSocketOpen&)> Handler);
	FVaRestSocketChannel OnReady(const UObject* Owner, TFunction<void(const FVaRestSocketOpen&)> Handler);
	FVaRestSocketChannel OnDisconnected(const UObject* Owner, TFunction<void(const FVaRestSocketClose&)> Handler);
	FVaRestSocketChannel OnMessage(const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler);
	FVaRestSocketChannel OnUnrouted(const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler);
	FVaRestSocketChannel On(FStringView Route, const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler);
	FVaRestSocketChannel On(FStringView Route, const UObject* Owner, TFunction<void(const FVaRestJson&)> Handler);
	template <typename T> FVaRestSocketChannel On(FStringView Route, const UObject* Owner, TFunction<void(const T&)> Handler);
	template <typename F> FVaRestSocketChannel On(FStringView Route, const UObject* Owner, F&& Handler);
	template <typename F> FVaRestSocketChannel On(FStringView Route, F&& Handler);
	template <typename T> FVaRestSocketChannel On(FStringView Route, TFunction<void(const T&)> Handler);
	FVaRestSocketChannel OnRequest(FStringView Route, const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler);
	template <typename T> FVaRestSocketChannel OnRequest(FStringView Route, const UObject* Owner, TFunction<void(const T&)> Handler);
	template <typename T, typename R> FVaRestSocketChannel OnRequest(FStringView Route, const UObject* Owner, TFunction<void(const T&, TVaRestSocketReply<R>&)> Handler);
	template <typename F> FVaRestSocketChannel OnRequest(FStringView Route, const UObject* Owner, F&& Handler);
	template <typename F> FVaRestSocketChannel OnRequest(FStringView Route, F&& Handler);
	FVaRestSocketChannel On(FStringView Route, TFunction<void(const FVaRestSocketMessage&)> Handler) { return On(Route, nullptr, MoveTemp(Handler)); }
	FVaRestSocketChannel On(FStringView Route, TFunction<void(const FVaRestJson&)> Handler) { return On(Route, nullptr, MoveTemp(Handler)); }
	FVaRestSocketChannel OnRequest(FStringView Route, TFunction<void(const FVaRestSocketMessage&)> Handler) { return OnRequest(Route, nullptr, MoveTemp(Handler)); }
	FVaRestSocketChannel OnConnected(TFunction<void(const FVaRestSocketOpen&)> Handler) { return OnConnected(nullptr, MoveTemp(Handler)); }
	FVaRestSocketChannel OnReady(TFunction<void(const FVaRestSocketOpen&)> Handler) { return OnReady(nullptr, MoveTemp(Handler)); }
	FVaRestSocketChannel OnDisconnected(TFunction<void(const FVaRestSocketClose&)> Handler) { return OnDisconnected(nullptr, MoveTemp(Handler)); }
	FVaRestSocketChannel OnMessage(TFunction<void(const FVaRestSocketMessage&)> Handler) { return OnMessage(nullptr, MoveTemp(Handler)); }
	FVaRestSocketChannel OnUnrouted(TFunction<void(const FVaRestSocketMessage&)> Handler) { return OnUnrouted(nullptr, MoveTemp(Handler)); }
	void Off(FStringView Route, const UObject* Owner);
	void OffAll(const UObject* Owner);
	/** Any thread: encode with this channel's published native snapshot. */
	EVaRestSendResult Emit(FStringView Route, const FVaRestJson& Payload, EVaRestSend Flags = EVaRestSend::Default);
	template <typename T> requires (VaRest::Json::bHasScriptStruct<T> && !std::is_same_v<T, FVaRestJson>)
	EVaRestSendResult Emit(FStringView Route, const T& Value, EVaRestSend Flags = EVaRestSend::Default)
	{ return Socket.EmitStruct(Name, Route, VaRest::Json::GetScriptStruct<T>(), &Value, Flags); }
	[[nodiscard]] FVaRestSocketJsonRequest Request(FStringView Route, FVaRestJson Payload);
	template <typename T> [[nodiscard]] TVaRestSocketRequest<T> Request(FStringView Route, FVaRestJson Payload);
	template <typename T = FVaRestJson, typename P> requires (VaRest::Json::bHasScriptStruct<P> && !std::is_same_v<P, FVaRestJson>)
	[[nodiscard]] TVaRestSocketRequest<T> Request(FStringView Route, const P& Payload);
	int32 GetQueuedCount() const;
	void ClearQueue();
private:
	friend class FVaRestSocket;
	FVaRestSocketChannel(FVaRestSocket InSocket, FString InName) : Socket(MoveTemp(InSocket)), Name(MoveTemp(InName)) {}
	FVaRestSocket Socket;
	FString Name;
};

#include "VaRestSocketTyped.h"
