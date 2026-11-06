// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestClient.h"
#include "VaRestSocketClientTypes.h"
#include "VaRestSocketMessage.h"
#include "VaRestSocketCodec.h"
#include "VaRestSocketStage.h"
#include "VaRestSocketStats.h"
#include "VaRestSocketTransport.h"

class UVaRestHub;
class UVaRestApi;
class UVaRestSocketHub;
class FVaRestSocketWeak;
class FVaRestSocketJsonRequest;
class FVaRestSocketChannel;
namespace VaRest::WebSocket { struct FSocketHandle; struct FSocketAccess; struct FSocketTypedAccess; }
template <typename T> class TVaRestSocketReply;
template <typename T> class TVaRestSocketRequest;

/** Scoped socket handle. Configuration/lifecycle/getters are GT; copies and final release may occur on any thread.
 * Only the native token crosses threads. Callbacks and the connection owner are disposed on GT before Hub teardown. */
class VARESTWEBSOCKET_API FVaRestSocket
{
public:
	FVaRestSocket() = default;
	bool IsValid() const;
	bool IsSameSocket(const FVaRestSocket& Other) const { return Handle && Handle == Other.Handle; }
	FName GetProtocolTag() const;
	/** Borrowed GT-only sibling-protocol view; its stage owns the actual UObject lifetime. */
	UObject* GetProtocolStateOnGT() const;
	bool SetProtocolStateOnGT(UObject* State);
	FVaRestSocketBatchPin GetProtocolDispatchPinOnGT(FStringView Channel) const;
	bool IsProtocolDispatchCurrentOnGT(const FVaRestSocketBatchPin& Pin) const;
	void ReportProtocolErrorOnGT(FVaRestError Error, FStringView Channel = {}) const;
	FVaRestSocketWeak AsWeak() const;
	FVaRestSocketChannel Channel(FStringView Name) const;
	FVaRestSocket WithUrl(FString Url);
	FVaRestSocket WithProtocols(TArray<FString> Protocols);
	FVaRestSocket WithHeader(FString Name, FString Value);
	FVaRestSocket RemoveHeader(FStringView Name);
	FVaRestSocket WithQuery(FString Name, FString Value);
	FVaRestSocket RemoveQuery(FStringView Name);
	FVaRestSocket WithOrigin(TOptional<FString> Origin);
	FVaRestSocket WithApi(UVaRestApi* Api);
	FVaRestSocket WithClient(FVaRestClient Client);
	/** Shares API conversion/redaction settings; false keeps session credentials and refresh disabled. */
	FVaRestSocket WithClient(FVaRestClient Client, bool bApplyCredentials);
	FVaRestSocket WithBearer(TFunction<FString()> Provider);
	FVaRestSocket WithAuthPlacement(FVaRestSocketAuthPlacement Placement);
	void ResetSession();
	FVaRestSocket WithReconnect(FVaRestReconnectPolicy Policy);
	FVaRestSocket NoReconnect();
	FVaRestSocket WithBackground(EVaRestSocketBackground Policy);
	FVaRestSocket WithLogLevel(EVaRestLogLevel Level);
	FVaRestSocket WithTimeouts(double ConnectSeconds, double CloseSeconds);
	FVaRestSocket ReadyWhen(FString Route);
	FVaRestSocket WithLimits(FVaRestSocketLimits Limits);
	FVaRestSocket WithQueue(FVaRestSocketQueuePolicy Policy);
	FVaRestSocket WithHeartbeat(FVaRestHeartbeat Heartbeat);
	FVaRestSocket WithProtocolPing(FVaRestProtocolPing Ping);
	/** Configure service-thread exact-text replies for subsequent attempts. */
	FVaRestSocket WithAutoReplies(TArray<FVaRestSocketAutoReply> Rules);
	FVaRestSocket WithRouting(FVaRestSocketRouting Routing);
	FVaRestSocket WithCodec(TSharedRef<IVaRestSocketCodec, ESPMode::ThreadSafe> Codec);
	FVaRestSocket WithNaming(EVaRestJsonNaming Naming);
	FVaRestSocket WithStrictDecode(bool bStrict = true);
	FVaRestSocket Use(TSharedRef<IVaRestSocketStage> Stage);
	FVaRestSocket Owner(const UObject* Owner);
	FVaRestSocket OnHandshake(const UObject* Owner, TFunction<void(FVaRestSocketHandshake&)> Handler);
	FVaRestSocket ReconnectIf(TFunction<TOptional<bool>(const FVaRestSocketClose&)> Predicate);
	FVaRestSocket OnConnected(const UObject* Owner, TFunction<void(const FVaRestSocketOpen&)> Handler);
	FVaRestSocket OnReady(const UObject* Owner, TFunction<void(const FVaRestSocketOpen&)> Handler);
	FVaRestSocket OnDisconnected(const UObject* Owner, TFunction<void(const FVaRestSocketClose&)> Handler);
	FVaRestSocket OnRetryScheduled(const UObject* Owner, TFunction<void(const FVaRestSocketStatus&, const FVaRestSocketClose&)> Handler);
	FVaRestSocket OnGaveUp(const UObject* Owner, TFunction<void(const FVaRestError&, const FVaRestSocketClose&)> Handler);
	FVaRestSocket OnStateChanged(const UObject* Owner, TFunction<void(EVaRestSocketState, EVaRestSocketState)> Handler);
	FVaRestSocket OnError(const UObject* Owner, TFunction<void(const FVaRestError&)> Handler);
	FVaRestSocket OnDrained(const UObject* Owner, TFunction<void()> Handler);
	FVaRestSocket OnMessage(const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler);
	FVaRestSocket OnBinary(const UObject* Owner, TFunction<void(TConstArrayView<uint8>, const FVaRestSocketMessage&)> Handler);
	FVaRestSocket OnUnrouted(const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler);
	FVaRestSocket On(FStringView Route, const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler);
	FVaRestSocket On(FStringView Route, const UObject* Owner, TFunction<void(const FVaRestJson&)> Handler);
	template <typename T> FVaRestSocket On(FStringView Route, const UObject* Owner, TFunction<void(const T&)> Handler);
	template <typename F> FVaRestSocket On(FStringView Route, const UObject* Owner, F&& Handler);
	template <typename F> FVaRestSocket On(FStringView Route, F&& Handler);
	template <typename T> FVaRestSocket On(FStringView Route, TFunction<void(const T&)> Handler);
	FVaRestSocket OnRequest(FStringView Route, const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler);
	template <typename T> FVaRestSocket OnRequest(FStringView Route, const UObject* Owner, TFunction<void(const T&)> Handler);
	template <typename T, typename R> FVaRestSocket OnRequest(FStringView Route, const UObject* Owner, TFunction<void(const T&, TVaRestSocketReply<R>&)> Handler);
	template <typename F> FVaRestSocket OnRequest(FStringView Route, const UObject* Owner, F&& Handler);
	template <typename F> FVaRestSocket OnRequest(FStringView Route, F&& Handler);
	FVaRestSocket OnRequest(FStringView Route, TFunction<void(const FVaRestSocketMessage&)> Handler) { return OnRequest(Route, nullptr, MoveTemp(Handler)); }
	FVaRestSocket On(FStringView Route, TFunction<void(const FVaRestSocketMessage&)> Handler) { return On(Route, nullptr, MoveTemp(Handler)); }
	FVaRestSocket On(FStringView Route, TFunction<void(const FVaRestJson&)> Handler) { return On(Route, nullptr, MoveTemp(Handler)); }
	void Off(FStringView Route, const UObject* Owner);
	FVaRestSocket OnConnected(TFunction<void(const FVaRestSocketOpen&)> Handler) { return OnConnected(nullptr, MoveTemp(Handler)); }
	FVaRestSocket OnReady(TFunction<void(const FVaRestSocketOpen&)> Handler) { return OnReady(nullptr, MoveTemp(Handler)); }
	FVaRestSocket OnDisconnected(TFunction<void(const FVaRestSocketClose&)> Handler) { return OnDisconnected(nullptr, MoveTemp(Handler)); }
	FVaRestSocket OnRetryScheduled(TFunction<void(const FVaRestSocketStatus&, const FVaRestSocketClose&)> Handler) { return OnRetryScheduled(nullptr, MoveTemp(Handler)); }
	FVaRestSocket OnGaveUp(TFunction<void(const FVaRestError&, const FVaRestSocketClose&)> Handler) { return OnGaveUp(nullptr, MoveTemp(Handler)); }
	FVaRestSocket OnStateChanged(TFunction<void(EVaRestSocketState, EVaRestSocketState)> Handler) { return OnStateChanged(nullptr, MoveTemp(Handler)); }
	FVaRestSocket OnError(TFunction<void(const FVaRestError&)> Handler) { return OnError(nullptr, MoveTemp(Handler)); }
	FVaRestSocket OnDrained(TFunction<void()> Handler) { return OnDrained(nullptr, MoveTemp(Handler)); }
	FVaRestSocket OnMessage(TFunction<void(const FVaRestSocketMessage&)> Handler) { return OnMessage(nullptr, MoveTemp(Handler)); }
	FVaRestSocket OnBinary(TFunction<void(TConstArrayView<uint8>, const FVaRestSocketMessage&)> Handler) { return OnBinary(nullptr, MoveTemp(Handler)); }
	FVaRestSocket OnUnrouted(TFunction<void(const FVaRestSocketMessage&)> Handler) { return OnUnrouted(nullptr, MoveTemp(Handler)); }
	void OffAll(const UObject* Owner);
	FVaRestSocket Connect();
	void Close(int32 Code = 1000, FStringView Reason = {});
	void ReconnectNow();
	void StopReconnecting();
	void Restart(FStringView Reason = TEXTVIEW("restart"));
	/** Text/JSON/binary/batches encode on the caller's thread, then use native bounded staging. */
	EVaRestSendResult Send(FStringView Text, EVaRestSend Flags = EVaRestSend::Default);
	EVaRestSendResult Send(FUtf8StringView Text, EVaRestSend Flags = EVaRestSend::Default);
	EVaRestSendResult Send(const FVaRestJson& Json, EVaRestSend Flags = EVaRestSend::Default);
	/** Synchronous caller-thread conversion; reflection, native defaults and hooks follow FVaRestJsonConvert's contract. */
	template <typename T> requires (VaRest::Json::bHasScriptStruct<T> && !std::is_same_v<T, FVaRestJson>)
	EVaRestSendResult Send(const T& Value, EVaRestSend Flags = EVaRestSend::Default)
	{ return SendStruct(VaRest::Json::GetScriptStruct<T>(), &Value, Flags); }
	EVaRestSendResult SendBinary(TArray<uint8>&& Bytes, EVaRestSend Flags = EVaRestSend::Default);
	EVaRestSendResult SendBatch(TArray<FVaRestSocketFrame> Frames, EVaRestSend Flags = EVaRestSend::Default, TOptional<FVaRestSocketBatchPin> Pin = {});
	/** Synchronous caller-thread protocol encoding under one codec/conversion/channel/queue snapshot.
	 * The function and borrowed values are never retained; only its complete encoded batch enters the outbox. */
	using FEncodeCall = TFunctionRef<TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError>(const IVaRestSocketCodec&,
		const FVaRestSocketCodecContext&, const FVaRestJsonConvertOptions&, FStringView)>;
	EVaRestSendResult EncodeAndSend(FStringView Channel, FEncodeCall Encode, EVaRestSend Flags = EVaRestSend::Default);
	/** Native sibling-protocol builder input; same synchronous snapshot, with no enqueue. */
	TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError> EncodeOnly(FStringView Channel, FEncodeCall Encode) const;
	EVaRestSendResult SendFrame(FVaRestSocketFrame Frame); // GT only
	EVaRestSendResult Emit(FStringView Route, const FVaRestJson& Payload, EVaRestSend Flags = EVaRestSend::Default);
	template <typename T> requires (VaRest::Json::bHasScriptStruct<T> && !std::is_same_v<T, FVaRestJson>)
	EVaRestSendResult Emit(FStringView Route, const T& Value, EVaRestSend Flags = EVaRestSend::Default)
	{ return EmitStruct({}, Route, VaRest::Json::GetScriptStruct<T>(), &Value, Flags); }
	/** GTK builder. The JSON handle is borrowed until Send; clone it if its producer will edit it meanwhile. */
	[[nodiscard]] FVaRestSocketJsonRequest Request(FStringView Route, FVaRestJson Payload);
	/** GTK-only sibling-protocol request producer; called once during Send, retired before native enqueue. */
	using FRequestFrameFactory = TUniqueFunction<TValueOrError<TArray<FVaRestSocketFrame>, FVaRestError>(const FVaRestSocketId&,
		const IVaRestSocketCodec&, const FVaRestSocketCodecContext&, const FVaRestJsonConvertOptions&)>;
	[[nodiscard]] FVaRestSocketJsonRequest RequestEncoded(FStringView Route, FRequestFrameFactory Frames);
	template <typename T> [[nodiscard]] TVaRestSocketRequest<T> Request(FStringView Route, FVaRestJson Payload);
	template <typename T = FVaRestJson, typename P> requires (VaRest::Json::bHasScriptStruct<P> && !std::is_same_v<P, FVaRestJson>)
	[[nodiscard]] TVaRestSocketRequest<T> Request(FStringView Route, const P& Payload);
	int32 GetQueuedCount() const;
	void ClearQueue();
	EVaRestSocketState GetState() const;
	FVaRestSocketStatus GetStatus() const;
	FVaRestSocketOpen GetLastOpen() const;
	FVaRestSocketStats GetStats() const;
	bool IsConnected() const { return GetState() == EVaRestSocketState::Connected; }
	bool IsReady() const { return GetStatus().bReady; }
	FName GetName() const;
	FString GetRedactedUrl() const;
	FString ScrubTextOnGT(FStringView Text) const;
	int64 GetBufferedAmount() const;
	double GetLatency() const;
	double GetLatencyAverage() const;
private:
	friend class UVaRestSocketClient;
	friend class UVaRestSocketLibrary;
	friend class UVaRestSocketRequestAction;
	friend struct VaRest::WebSocket::FSocketTypedAccess;
	template <typename T> friend class TVaRestSocketRequest;
	friend class FVaRestSocketWeak;
	friend struct VaRest::WebSocket::FSocketAccess;
	friend struct VaRest::WebSocket::FSocketReplyToken;
	friend struct VaRest::WebSocket::FSocketData;
	friend class FVaRestSocketJsonRequest;
	friend class FVaRestSocketChannel;
	explicit FVaRestSocket(TSharedPtr<VaRest::WebSocket::FSocketHandle, ESPMode::ThreadSafe> InHandle) : Handle(MoveTemp(InHandle)) {}
	EVaRestSendResult SendEncoded(TArray<FVaRestSocketFrame> Frames, EVaRestSend Flags, uint64 Epoch, TOptional<FVaRestSocketBatchPin> Pin = {}, FString Channel = {}, uint64 QueueEpoch = 0);
	EVaRestSendResult SendStruct(const UScriptStruct* Struct, const void* Value, EVaRestSend Flags);
	EVaRestSendResult EmitStruct(FStringView Channel, FStringView Route, const UScriptStruct* Struct, const void* Value, EVaRestSend Flags);
	/** Blueprint wildcard inputs are borrowed synchronously on GT, then only the encoded bytes are admitted. */
	EVaRestSendResult SendPropertyOnGT(const FProperty* Property, const void* Value, EVaRestSend Flags);
	EVaRestSendResult EmitPropertyOnGT(FStringView Route, const FProperty* Property, const void* Value, EVaRestSend Flags);
	FVaRestSocketBatchPin CaptureDispatchPinOnGT(FStringView Channel) const;
	bool IsDispatchPinCurrentOnGT(const FVaRestSocketBatchPin& Pin) const;
	FVaRestJsonConvertOptions GetConversionOptionsOnGT() const;
	void ReportDecodeFailureOnGT(const FVaRestJsonDecodeReport& Report);
	FVaRestError MakeDecodeFailureOnGT(const FVaRestJsonDecodeReport& Report) const;
	FVaRestSocketJsonRequest RequestStructSnapshotOnGT(FStringView Route, const UScriptStruct* Struct, const void* Value);
	FVaRestSocketJsonRequest PreparePropertyRequestOnGT(FStringView Route, FStringView Channel, const FProperty* Property, const void* Value);
	TSharedPtr<VaRest::WebSocket::FSocketHandle, ESPMode::ThreadSafe> Handle;
};

/** Pure native weak token; Pin does not access reflection or retain an HTTP client. */
class VARESTWEBSOCKET_API FVaRestSocketWeak
{
public:
	FVaRestSocketWeak() = default;
	FVaRestSocket Pin() const;
private:
	friend class FVaRestSocket;
	TWeakPtr<VaRest::WebSocket::FSocketHandle, ESPMode::ThreadSafe> Handle;
};

#include "VaRestSocketRequest.h"
#include "VaRestSocketChannel.h"
