// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestSocket.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"
#include <type_traits>

/** Pure native reply token. Copies and Send may cross threads under the ordinary synchronous payload-encode contract. */
template <typename T>
class TVaRestSocketReply
{
public:
	explicit TVaRestSocketReply(FVaRestSocketMessage InMessage) : Message(MoveTemp(InMessage)) {}
	EVaRestSendResult Send(const T& Value) const
	{
		if constexpr (std::is_same_v<T, FString>) return Message.Reply(FVaRestJson::From(Value));
		else return Message.Reply(Value);
	}
	EVaRestSendResult Error(FString Code, FString Text) const { return Message.ReplyError(MoveTemp(Code), MoveTemp(Text)); }
	EVaRestSendResult Error(int64 Code, FString Text) const { return Message.ReplyError(Code, MoveTemp(Text)); }
private:
	FVaRestSocketMessage Message;
};

namespace VaRest::WebSocket
{
	/** Access is synchronous GTK-only. None of these methods publishes a reflected descriptor or typed carrier. */
	struct FSocketTypedAccess
	{
		static FVaRestSocketBatchPin Pin(const FVaRestSocket& Socket, FStringView Channel) { return Socket.CaptureDispatchPinOnGT(Channel); }
		static bool Current(const FVaRestSocket& Socket, const FVaRestSocketBatchPin& DispatchPin) { return Socket.IsDispatchPinCurrentOnGT(DispatchPin); }
		static FVaRestJsonConvertOptions Options(const FVaRestSocket& Socket) { return Socket.GetConversionOptionsOnGT(); }
		static void Failed(FVaRestSocket& Socket, const FVaRestJsonDecodeReport& Report) { Socket.ReportDecodeFailureOnGT(Report); }
	};
	namespace SocketTypedPrivate
	{
		template <typename... A> struct TArguments;
		template <> struct TArguments<> { using First = void; using Second = void; static constexpr int32 Arity = 0; };
		template <typename A> struct TArguments<A> { using First = std::remove_cvref_t<A>; using Second = void; static constexpr int32 Arity = 1; };
		template <typename A, typename B, typename... Rest> struct TArguments<A, B, Rest...>
		{ using First = std::remove_cvref_t<A>; using Second = std::remove_cvref_t<B>; static constexpr int32 Arity = 2 + sizeof...(Rest); };
		template <typename F> struct TSignature : TArguments<> {};
		template <typename R, typename C, typename... A> struct TSignature<R(C::*)(A...) const> : TArguments<A...> {};
		template <typename R, typename C, typename... A> struct TSignature<R(C::*)(A...)> : TArguments<A...> {};
		template <typename R, typename C, typename... A> struct TSignature<R(C::*)(A...) const noexcept> : TArguments<A...> {};
		template <typename R, typename C, typename... A> struct TSignature<R(C::*)(A...) noexcept> : TArguments<A...> {};
		template <typename R, typename... A> struct TSignature<R(*)(A...)> : TArguments<A...> {};
		template <typename R, typename... A> struct TSignature<R(*)(A...) noexcept> : TArguments<A...> {};
		template <typename F, typename = void> struct TCallable : TSignature<std::decay_t<F>> {};
		template <typename F> struct TCallable<F, std::void_t<decltype(&std::decay_t<F>::operator())>> : TSignature<decltype(&std::decay_t<F>::operator())> {};
		template <typename R> struct TReply { using Value = void; static constexpr bool Valid = false; };
		template <typename R> struct TReply<TVaRestSocketReply<R>> { using Value = R; static constexpr bool Valid = true; };
		template <typename T> inline constexpr bool Supported = std::is_same_v<T, FVaRestSocketMessage> || std::is_same_v<T, FVaRestJson>
			|| std::is_same_v<T, FString> || VaRest::Json::bHasScriptStruct<T>;

		/** Safe GTK materialization adapter. JSON parsing happens on the native pipe. A frozen native typed-decode plan
		 * remains an open W4 requirement: no arbitrary T constructor, destructor or reflected walk runs on a worker. */
		template <typename T, typename F>
		void Invoke(const FVaRestSocketWeak& Weak, const TWeakObjectPtr<const UObject>& Owner, bool bOwned, const FVaRestSocketMessage& Incoming, F& Handler)
		{
			check(IsInGameThread());
			static_assert(Supported<T>, "Socket handlers take a USTRUCT, FVaRestJson, FString or FVaRestSocketMessage");
			auto Socket = Weak.Pin(); if (!Socket.IsValid() || (bOwned && !Owner.IsValid())) return;
			const auto Pin = FSocketTypedAccess::Pin(Socket, Incoming.Channel);
			if (!FSocketTypedAccess::Current(Socket, Pin)) return;
			if constexpr (std::is_same_v<T, FVaRestSocketMessage>) Handler(Incoming);
			else if constexpr (std::is_same_v<T, FVaRestJson>) Handler(Incoming.Data());
			else if constexpr (std::is_same_v<T, FString>)
			{
				const FString Text = Incoming.Text();
				if ((!bOwned || Owner.IsValid()) && FSocketTypedAccess::Current(Socket, Pin)) Handler(Text);
			}
			else if constexpr (VaRest::Json::bHasScriptStruct<T>)
			{
				auto Options = FSocketTypedAccess::Options(Socket);
				if (!Incoming.Attachments.IsEmpty()) Options.BytesResolver = MakePlaceholderBytesReader(Incoming.Attachments);
				const TStrongObjectPtr<const UScriptStruct> Type(VaRest::Json::GetScriptStruct<T>());
				if (!Type.Get()) return;
				T Value{};
				if ((bOwned && !Owner.IsValid()) || !FSocketTypedAccess::Current(Socket, Pin)) return;
				FVaRestJsonDecodeReport Report;
				const bool bDecoded = FVaRestJsonConvert::JsonToStruct(Incoming.Data(), Type.Get(), &Value, Options, &Report);
				if ((bOwned && !Owner.IsValid()) || !FSocketTypedAccess::Current(Socket, Pin)) return;
				if (bDecoded) Handler(Value); else FSocketTypedAccess::Failed(Socket, Report);
			}
		}
		template <typename T> TFunction<void(const FVaRestSocketMessage&)> MessageHandler(FVaRestSocketWeak Weak, const UObject* Owner, TFunction<void(const T&)> Handler)
		{
			if (!Handler) return {};
			const TWeakObjectPtr<const UObject> WeakOwner(Owner);
			return [Weak, WeakOwner, bOwned = Owner != nullptr, Handler = MoveTemp(Handler)](const FVaRestSocketMessage& Incoming) mutable
			{ Invoke<T>(Weak, WeakOwner, bOwned, Incoming, Handler); };
		}
		template <typename T, typename R> TFunction<void(const FVaRestSocketMessage&)> RequestHandler(FVaRestSocketWeak Weak, const UObject* Owner, TFunction<void(const T&, TVaRestSocketReply<R>&)> Handler)
		{
			if (!Handler) return {};
			const TWeakObjectPtr<const UObject> WeakOwner(Owner);
			return [Weak, WeakOwner, bOwned = Owner != nullptr, Handler = MoveTemp(Handler)](const FVaRestSocketMessage& Incoming) mutable
			{
				auto Deliver = [&Incoming, &Handler](const T& Value) { TVaRestSocketReply<R> Reply(Incoming); Handler(Value, Reply); };
				Invoke<T>(Weak, WeakOwner, bOwned, Incoming, Deliver);
			};
		}
	}
}

template <typename T> FVaRestSocket FVaRestSocket::On(FStringView Route, const UObject* Owner, TFunction<void(const T&)> Handler)
{
	check(IsInGameThread());
	return On(Route, Owner, VaRest::WebSocket::SocketTypedPrivate::MessageHandler<T>(AsWeak(), Owner, MoveTemp(Handler)));
}
template <typename F> FVaRestSocket FVaRestSocket::On(FStringView Route, const UObject* Owner, F&& Handler)
{
	using Signature = VaRest::WebSocket::SocketTypedPrivate::TCallable<F>;
	static_assert(Signature::Arity == 1, "Name the socket handler's single argument type; generic/overloaded lambdas need an explicit On<T> adapter");
	if constexpr (Signature::Arity == 1)
	{
		using Value = typename Signature::First;
		return On<Value>(Route, Owner, TFunction<void(const Value&)>(Forward<F>(Handler)));
	}
	return *this;
}
template <typename F> FVaRestSocket FVaRestSocket::On(FStringView Route, F&& Handler) { return On(Route, nullptr, Forward<F>(Handler)); }
template <typename T> FVaRestSocket FVaRestSocket::On(FStringView Route, TFunction<void(const T&)> Handler) { return On<T>(Route, nullptr, MoveTemp(Handler)); }
template <typename T> FVaRestSocket FVaRestSocket::OnRequest(FStringView Route, const UObject* Owner, TFunction<void(const T&)> Handler)
{
	check(IsInGameThread()); return OnRequest(Route, Owner, VaRest::WebSocket::SocketTypedPrivate::MessageHandler<T>(AsWeak(), Owner, MoveTemp(Handler)));
}
template <typename T, typename R> FVaRestSocket FVaRestSocket::OnRequest(FStringView Route, const UObject* Owner, TFunction<void(const T&, TVaRestSocketReply<R>&)> Handler)
{
	check(IsInGameThread());
	return OnRequest(Route, Owner, VaRest::WebSocket::SocketTypedPrivate::RequestHandler<T, R>(AsWeak(), Owner, MoveTemp(Handler)));
}
template <typename F> FVaRestSocket FVaRestSocket::OnRequest(FStringView Route, const UObject* Owner, F&& Handler)
{
	using Signature = VaRest::WebSocket::SocketTypedPrivate::TCallable<F>;
	static_assert(Signature::Arity == 1 || Signature::Arity == 2, "Name the request argument and TVaRestSocketReply<R>& types; generic lambdas need explicit types");
	if constexpr (Signature::Arity == 1)
		return OnRequest<typename Signature::First>(Route, Owner, TFunction<void(const typename Signature::First&)>(Forward<F>(Handler)));
	else if constexpr (Signature::Arity == 2)
	{
		using Reply = VaRest::WebSocket::SocketTypedPrivate::TReply<typename Signature::Second>;
		static_assert(Reply::Valid, "The second OnRequest argument must be TVaRestSocketReply<R>&");
		if constexpr (Reply::Valid)
			return OnRequest<typename Signature::First, typename Reply::Value>(Route, Owner, Forward<F>(Handler));
	}
	return *this;
}
template <typename F> FVaRestSocket FVaRestSocket::OnRequest(FStringView Route, F&& Handler) { return OnRequest(Route, nullptr, Forward<F>(Handler)); }

template <typename T> FVaRestSocketChannel FVaRestSocketChannel::On(FStringView Route, const UObject* Owner, TFunction<void(const T&)> Handler)
{
	check(IsInGameThread());
	return On(Route, Owner, VaRest::WebSocket::SocketTypedPrivate::MessageHandler<T>(Socket.AsWeak(), Owner, MoveTemp(Handler)));
}
template <typename F> FVaRestSocketChannel FVaRestSocketChannel::On(FStringView Route, const UObject* Owner, F&& Handler)
{
	using Signature = VaRest::WebSocket::SocketTypedPrivate::TCallable<F>;
	static_assert(Signature::Arity == 1, "Name the channel handler's single argument type; generic lambdas need explicit On<T>");
	if constexpr (Signature::Arity == 1)
		return On<typename Signature::First>(Route, Owner, TFunction<void(const typename Signature::First&)>(Forward<F>(Handler)));
	return *this;
}
template <typename F> FVaRestSocketChannel FVaRestSocketChannel::On(FStringView Route, F&& Handler) { return On(Route, nullptr, Forward<F>(Handler)); }
template <typename T> FVaRestSocketChannel FVaRestSocketChannel::On(FStringView Route, TFunction<void(const T&)> Handler) { return On<T>(Route, nullptr, MoveTemp(Handler)); }
template <typename T> FVaRestSocketChannel FVaRestSocketChannel::OnRequest(FStringView Route, const UObject* Owner, TFunction<void(const T&)> Handler)
{
	check(IsInGameThread()); return OnRequest(Route, Owner, VaRest::WebSocket::SocketTypedPrivate::MessageHandler<T>(Socket.AsWeak(), Owner, MoveTemp(Handler)));
}
template <typename T, typename R> FVaRestSocketChannel FVaRestSocketChannel::OnRequest(FStringView Route, const UObject* Owner, TFunction<void(const T&, TVaRestSocketReply<R>&)> Handler)
{
	check(IsInGameThread());
	return OnRequest(Route, Owner, VaRest::WebSocket::SocketTypedPrivate::RequestHandler<T, R>(Socket.AsWeak(), Owner, MoveTemp(Handler)));
}
template <typename F> FVaRestSocketChannel FVaRestSocketChannel::OnRequest(FStringView Route, const UObject* Owner, F&& Handler)
{
	using Signature = VaRest::WebSocket::SocketTypedPrivate::TCallable<F>;
	static_assert(Signature::Arity == 1 || Signature::Arity == 2, "Name the channel request argument and TVaRestSocketReply<R>& types");
	if constexpr (Signature::Arity == 1)
		return OnRequest<typename Signature::First>(Route, Owner, TFunction<void(const typename Signature::First&)>(Forward<F>(Handler)));
	else if constexpr (Signature::Arity == 2)
	{
		using Reply = VaRest::WebSocket::SocketTypedPrivate::TReply<typename Signature::Second>;
		static_assert(Reply::Valid, "The second OnRequest argument must be TVaRestSocketReply<R>&");
		if constexpr (Reply::Valid)
			return OnRequest<typename Signature::First, typename Reply::Value>(Route, Owner, Forward<F>(Handler));
	}
	return *this;
}
template <typename F> FVaRestSocketChannel FVaRestSocketChannel::OnRequest(FStringView Route, F&& Handler) { return OnRequest(Route, nullptr, Forward<F>(Handler)); }

// These return types are dependent, so direct Request.h/Channel.h include orders finish their definitions before use.
template <typename T> TVaRestSocketRequest<T> FVaRestSocketChannel::Request(FStringView Route, FVaRestJson Payload)
{
	check(IsInGameThread()); auto Builder = Socket.Request<T>(Route, MoveTemp(Payload)); Builder.OnChannel(GetName()); return Builder;
}
template <typename T, typename P> requires (VaRest::Json::bHasScriptStruct<P> && !std::is_same_v<P, FVaRestJson>)
TVaRestSocketRequest<T> FVaRestSocketChannel::Request(FStringView Route, const P& Payload)
{
	check(IsInGameThread()); auto Builder = Socket.Request<T>(Route, Payload); Builder.OnChannel(GetName()); return Builder;
}
