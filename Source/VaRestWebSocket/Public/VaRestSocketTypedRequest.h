// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestSocketRequest.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"
#include <type_traits>

/** Move-only GTK builder. Native struct input is an owned GC snapshot, encoded once by Send().
 * Replies currently materialize on GTK through the core converter; frozen native worker plans remain an open gate.
 * Admission errors follow the JSON builder's synchronous GTK contract. Admitted results use the socket tick. */
template <typename T>
class [[nodiscard]] TVaRestSocketRequest
{
	static_assert(std::is_same_v<T, FVaRestSocketMessage> || std::is_same_v<T, FVaRestJson> || std::is_same_v<T, FString>
		|| VaRest::Json::bHasScriptStruct<T>, "Socket replies are USTRUCT, FVaRestJson, FString or FVaRestSocketMessage");
public:
	TVaRestSocketRequest(TVaRestSocketRequest&& Other) : Native(MoveTemp(Other.Native)), Callbacks(MoveTemp(Other.Callbacks)) { check(IsInGameThread()); }
	TVaRestSocketRequest& operator=(TVaRestSocketRequest&& Other)
	{ check(IsInGameThread()); if (this != &Other) { Native = MoveTemp(Other.Native); Callbacks = MoveTemp(Other.Callbacks); } return *this; }
	~TVaRestSocketRequest() { check(IsInGameThread()); }
	TVaRestSocketRequest(const TVaRestSocketRequest&) = delete;
	TVaRestSocketRequest& operator=(const TVaRestSocketRequest&) = delete;
	TVaRestSocketRequest& Timeout(double Seconds) { check(IsInGameThread()); Native.Timeout(Seconds); return *this; }
	TVaRestSocketRequest& Owner(const UObject* Object)
	{ check(IsInGameThread()); Native.Owner(Object); if (Callbacks) { Callbacks->Owner = Object; Callbacks->bOwned = Object != nullptr; } return *this; }
	TVaRestSocketRequest& OnSuccess(TFunction<void(const T&)> Handler)
	{ check(IsInGameThread()); if (Callbacks) Callbacks->Success = MoveTemp(Handler); return *this; }
	TVaRestSocketRequest& OnError(TFunction<void(const FVaRestError&)> Handler)
	{ check(IsInGameThread()); if (Callbacks) Callbacks->Error = MoveTemp(Handler); return *this; }
	FVaRestSocketRequestHandle Send()
	{
		check(IsInGameThread());
		auto State = MoveTemp(Callbacks); if (!State) return {};
		if (auto Socket = State->Socket.Pin(); Socket.IsValid()) State->Options = Socket.GetConversionOptionsOnGT();
		auto Sending = MoveTemp(Native); // no member is touched after a synchronous refusal calls user code
		Sending.SetMessageSuccessOnGT([State](const FVaRestSocketMessage& Message) { State->Deliver(Message); });
		Sending.OnError([State](const FVaRestError& Failure) { State->Fail(Failure); });
		return Sending.Send();
	}
private:
	friend class FVaRestSocket;
	friend class FVaRestSocketChannel;
	struct FCallbacks
	{
		~FCallbacks() { check(IsInGameThread()); }
		FVaRestSocketWeak Socket;
		FString Channel;
		FVaRestJsonConvertOptions Options;
		TWeakObjectPtr<const UObject> Owner;
		bool bOwned = false;
		TFunction<void(const T&)> Success;
		TFunction<void(const FVaRestError&)> Error;
		bool Alive() const { return !bOwned || Owner.IsValid(); }
		void Fail(const FVaRestError& Failure) { check(IsInGameThread()); if (Alive() && Error) Error(Failure); }
		void Deliver(const FVaRestSocketMessage& Message)
		{
			check(IsInGameThread()); auto Current = Socket.Pin(); if (!Alive() || !Success || !Current.IsValid()) return;
			const auto Pin = Current.CaptureDispatchPinOnGT(Message.Channel);
			if (!Current.IsDispatchPinCurrentOnGT(Pin)) return;
			if constexpr (std::is_same_v<T, FVaRestSocketMessage>) Success(Message);
			else if constexpr (std::is_same_v<T, FVaRestJson>) Success(Message.Data());
			else if constexpr (std::is_same_v<T, FString>)
			{
				const FString Text = Message.Text(); if (Alive() && Current.IsDispatchPinCurrentOnGT(Pin)) Success(Text);
			}
			else
			{
				const TStrongObjectPtr<const UScriptStruct> Type(VaRest::Json::GetScriptStruct<T>());
				FVaRestJsonDecodeReport Report;
				if (!Type.Get()) { Fail(Current.MakeDecodeFailureOnGT(Report)); return; }
				T Value{};
				if (!Alive() || !Current.IsDispatchPinCurrentOnGT(Pin)) return;
				auto Effective = Options;
				if (!Message.Attachments.IsEmpty()) Effective.BytesResolver = VaRest::WebSocket::MakePlaceholderBytesReader(Message.Attachments);
				const bool bDecoded = FVaRestJsonConvert::JsonToStruct(Message.Data(), Type.Get(), &Value, Effective, &Report);
				if (!Alive() || !Current.IsDispatchPinCurrentOnGT(Pin)) return;
				if (bDecoded) Success(Value); else Fail(Current.MakeDecodeFailureOnGT(Report));
			}
		}
	};
	TVaRestSocketRequest(FVaRestSocketJsonRequest InNative, FVaRestSocketWeak Weak)
		: Native(MoveTemp(InNative)), Callbacks(MakeShared<FCallbacks>())
	{ check(IsInGameThread()); Callbacks->Socket = MoveTemp(Weak); }
	TVaRestSocketRequest& OnChannel(FString Channel)
	{ Native.SetChannelOnGT(Channel); Callbacks->Channel = MoveTemp(Channel); return *this; }
	FVaRestSocketJsonRequest Native;
	TSharedPtr<FCallbacks> Callbacks; // GTK-only; never shared with parser, transport, outbox or request handle
};

template <typename T> TVaRestSocketRequest<T> FVaRestSocket::Request(FStringView Route, FVaRestJson Payload)
{
	check(IsInGameThread()); return TVaRestSocketRequest<T>(Request(Route, MoveTemp(Payload)), AsWeak());
}
template <typename T, typename P> requires (VaRest::Json::bHasScriptStruct<P> && !std::is_same_v<P, FVaRestJson>)
TVaRestSocketRequest<T> FVaRestSocket::Request(FStringView Route, const P& Payload)
{
	check(IsInGameThread()); return TVaRestSocketRequest<T>(RequestStructSnapshotOnGT(Route, VaRest::Json::GetScriptStruct<P>(), &Payload), AsWeak());
}
