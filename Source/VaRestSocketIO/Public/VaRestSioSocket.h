// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestSocket.h"
#include "VaRestSioTypes.h"
#include "VaRestSioArguments.h"
#include "VaRestSioEvent.h"
#include "VaRestSioStats.h"

class UVaRestSioState;
class UVaRestSocketHub;
class FVaRestSioNamespace;
class FVaRestSioSocketWeak;
class FVaRestSioEmitWithAck;
class FVaRestSioEmitter;
class UVaRestSioSettings;
struct FVaRestSioStageOptions;

/** Copies and final release are any-thread. Only a native WS token and a weak UObject reference cross threads. */
class VARESTSOCKETIO_API FVaRestSioSocket
{
public:
	FVaRestSioSocket() = default;
	bool IsValid() const { return Native.IsValid(); }
	FVaRestSioSocketWeak AsWeak() const;
	FVaRestSioNamespace Namespace(FStringView Name) const;
	FVaRestSioNamespace DefaultNamespace() const;
	FVaRestSioEmitter Volatile() const;
	FVaRestSioSocket WithPath(FString Path);
	FVaRestSioSocket WithQuery(FString Key, FString Value);
	FVaRestSioSocket WithHeader(FString Name, FString Value);
	FVaRestSioSocket WithOrigin(TOptional<FString> Origin);
	FVaRestSioSocket WithApi(UVaRestApi* Api);
	FVaRestSioSocket WithClient(FVaRestClient Client);
	FVaRestSioSocket WithBearer(TFunction<FString()> Token);
	FVaRestSioSocket WithAuthPlacement(FVaRestSioAuthPlacement Placement);
	FVaRestSioSocket WithReconnect(FVaRestReconnectPolicy Policy);
	FVaRestSioSocket NoReconnect();
	FVaRestSioSocket WithRecovery(bool bEnabled);
	FVaRestSioSocket WithAckTimeout(double Seconds);
	FVaRestSioSocket WithProtocolPing(double Interval, double Timeout);
	FVaRestSioSocket WithQueue(FVaRestSocketQueuePolicy Policy);
	FVaRestSioSocket WithLimits(FVaRestSioLimits Limits);
	FVaRestSioSocket WithBackground(EVaRestSocketBackground Policy);
	FVaRestSioSocket WithNaming(EVaRestJsonNaming Naming);
	FVaRestSioSocket WithStrictDecode(bool bStrict = true);
	FVaRestSioSocket WithLogLevel(EVaRestLogLevel Level);
	FVaRestSioSocket Owner(const UObject* Owner);
	FVaRestSioSocket OnRetryScheduled(const UObject* Owner, TFunction<void(const FVaRestSocketStatus&, const FVaRestSocketClose&)> Handler);
	FVaRestSioSocket OnGaveUp(const UObject* Owner, TFunction<void(const FVaRestError&, const FVaRestSocketClose&)> Handler);
	FVaRestSioSocket OnStateChanged(const UObject* Owner, TFunction<void(EVaRestSocketState, EVaRestSocketState)> Handler);
	FVaRestSioSocket OnError(const UObject* Owner, TFunction<void(const FVaRestError&)> Handler);
	FVaRestSioSocket OnConnect(const UObject* Owner, TFunction<void(const FVaRestSioConnect&)> Handler);
	FVaRestSioSocket OnConnectError(const UObject* Owner, TFunction<void(const FVaRestError&, const FVaRestSioConnectError&)> Handler);
	FVaRestSioSocket OnDisconnect(const UObject* Owner, TFunction<void(const FVaRestSioDisconnect&)> Handler);
	FVaRestSioSocket OnConnect(TFunction<void(const FVaRestSioConnect&)> Handler) { return OnConnect(nullptr, MoveTemp(Handler)); }
	FVaRestSioSocket OnError(TFunction<void(const FVaRestError&)> Handler) { return OnError(nullptr, MoveTemp(Handler)); }
	FVaRestSioSocket WithAuth(FVaRestJson Payload);
	FVaRestSioSocket WithSessionAuth(bool bSend);
	FVaRestSioSocket OnConnectAuth(const UObject* Owner, TFunction<void(FVaRestJson&, const FVaRestSioConnectContext&)> Handler);
	FVaRestSioSocket ConnectErrorIf(TFunction<EVaRestSioRetry(const FVaRestSioConnectError&)> Predicate);
	FVaRestSioSocket On(FStringView Event, const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler);
	FVaRestSioSocket Once(FStringView Event, const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler);
	FVaRestSioSocket OnAny(const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler);
	FVaRestSioSocket OnUnhandled(const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler);
	FVaRestSioSocket On(FStringView Event, TFunction<void(const FVaRestSioEvent&)> Handler) { return On(Event, nullptr, MoveTemp(Handler)); }
	template <typename F> FVaRestSioSocket On(FStringView Event, const UObject* Owner, F&& Handler);
	template <typename F> FVaRestSioSocket Once(FStringView Event, const UObject* Owner, F&& Handler);
	template <typename F> FVaRestSioSocket On(FStringView Event, F&& Handler);
	template <typename F> FVaRestSioSocket Once(FStringView Event, F&& Handler);
	template <typename T, typename F> FVaRestSioSocket OnArgs(FStringView Event, const UObject* Owner, F&& Handler);
	template <typename T, typename F> FVaRestSioSocket OnArgs(FStringView Event, F&& Handler);
	void Off(FStringView Event, const UObject* Owner);
	void OffAny(const UObject* Owner);
	void OffAll(const UObject* Owner);
	FVaRestSioSocket Connect();
	void Close();
	void ReconnectNow();
	void StopReconnecting();
	void ResetSession();
	EVaRestSocketState GetState() const { return Native.GetState(); }
	FVaRestSocketStatus GetStatus() const { return Native.GetStatus(); }
	double GetLatency() const { return Native.GetLatency(); }
	FName GetName() const { return Native.GetName(); }
	FString GetRedactedUrl() const { return Native.GetRedactedUrl(); }
	/** Native transport counters; Socket.IO-specific counters are provided separately. */
	FVaRestSocketStats GetTransportStats() const { return Native.GetStats(); }
	/** GTK snapshot; an empty namespace selects the whole socket. */
	FVaRestSioStats GetStats(FStringView Namespace = {}) const;
	template <typename... A> EVaRestSendResult Emit(FStringView Event, A&&... Args);
	template <typename T> EVaRestSendResult EmitArgs(FStringView Event, const T& Args);
	template <typename... A> [[nodiscard]] FVaRestSioEmitWithAck EmitWithAck(FStringView Event, A&&... Args);
private:
	friend class FVaRestSio;
	friend class UVaRestSioClient;
	friend class FVaRestSioSocketWeak;
	friend class FVaRestSioNamespace;
	friend class FVaRestSioEmitWithAck;
	FVaRestSioSocket(FVaRestSocket Socket, UVaRestSioState* InState);
	UVaRestSioState* StateOnGT() const;
	EVaRestSendResult EmitArguments(FStringView Namespace, FStringView Event, FVaRestSocketPayloadWriter::FWriteArguments Arguments, EVaRestSend Flags) const;
	EVaRestSendResult EmitStructArguments(FStringView Namespace, FStringView Event, const UScriptStruct* Struct, const void* Value, EVaRestSend Flags) const;
	FVaRestSioEmitWithAck MakeAckBuilder(FStringView Namespace, FStringView Event, FVaRestSocketArgumentsWriter Arguments) const;
	FVaRestSocket Native;
	TWeakObjectPtr<UVaRestSioState> State;
};

class VARESTSOCKETIO_API FVaRestSioSocketWeak
{
public:
	FVaRestSioSocket Pin() const;
private:
	friend class FVaRestSioSocket;
	FVaRestSocketWeak Native;
	TWeakObjectPtr<UVaRestSioState> State;
};

/** One namespace retains the same native socket lifetime; configuration and observers are GT-only. */
class VARESTSOCKETIO_API FVaRestSioNamespace
{
public:
	FVaRestSioNamespace() = default;
	bool IsValid() const { return Socket.IsValid() && !Name.IsEmpty(); }
	FStringView GetName() const { return Name; }
	FVaRestSioNamespace WithAuth(FVaRestJson Payload);
	FVaRestSioNamespace WithSessionAuth(bool bSend);
	FVaRestSioNamespace WithConnectOnStart(bool bConnect);
	FVaRestSioNamespace OnConnectAuth(const UObject* Owner, TFunction<void(FVaRestJson&, const FVaRestSioConnectContext&)> Handler);
	FVaRestSioNamespace ConnectErrorIf(TFunction<EVaRestSioRetry(const FVaRestSioConnectError&)> Predicate);
	FVaRestSioNamespace OnConnect(const UObject* Owner, TFunction<void(const FVaRestSioConnect&)> Handler);
	FVaRestSioNamespace OnConnectError(const UObject* Owner, TFunction<void(const FVaRestError&, const FVaRestSioConnectError&)> Handler);
	FVaRestSioNamespace OnDisconnect(const UObject* Owner, TFunction<void(const FVaRestSioDisconnect&)> Handler);
	FVaRestSioNamespace On(FStringView Event, const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler);
	FVaRestSioNamespace On(FStringView Event, const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler);
	FVaRestSioNamespace Once(FStringView Event, const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler);
	FVaRestSioNamespace Once(FStringView Event, const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler);
	FVaRestSioNamespace OnAny(const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler);
	FVaRestSioNamespace OnAny(const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler);
	FVaRestSioNamespace OnUnhandled(const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)> Handler);
	FVaRestSioNamespace OnUnhandled(const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler);
	FVaRestSioNamespace OnConnect(TFunction<void(const FVaRestSioConnect&)> Handler) { return OnConnect(nullptr, MoveTemp(Handler)); }
	FVaRestSioNamespace On(FStringView Event, TFunction<void(const FVaRestSocketMessage&)> Handler) { return On(Event, nullptr, MoveTemp(Handler)); }
	template <typename F> FVaRestSioNamespace On(FStringView Event, const UObject* Owner, F&& Handler);
	template <typename F> FVaRestSioNamespace Once(FStringView Event, const UObject* Owner, F&& Handler);
	template <typename F> FVaRestSioNamespace On(FStringView Event, F&& Handler);
	template <typename F> FVaRestSioNamespace Once(FStringView Event, F&& Handler);
	template <typename T, typename F> FVaRestSioNamespace OnArgs(FStringView Event, const UObject* Owner, F&& Handler);
	template <typename T, typename F> FVaRestSioNamespace OnArgs(FStringView Event, F&& Handler);
	void Off(FStringView Event, const UObject* Owner);
	void OffAny(const UObject* Owner);
	void OffAll(const UObject* Owner);
	FVaRestSioNamespace Connect();
	void Disconnect();
	EVaRestSioNamespaceState GetState() const;
	bool IsConnected() const { return GetState() == EVaRestSioNamespaceState::Connected; }
	FString GetSid() const;
	bool WasRecovered() const;
	FVaRestSioStats GetStats() const { return Socket.GetStats(Name); }
	int32 GetQueuedCount() const;
	void ClearQueue();
	template <typename... A> EVaRestSendResult Emit(FStringView Event, A&&... Args) const
	{
		auto Write = [&](auto& Writer, const FVaRestJsonConvertOptions& Options, FVaRestJsonError& Error, TFunctionRef<bool()> WithinBudget)
		{ VaRest::Sio::FArgumentWriter Into{Writer, Options, Error, WithinBudget}; return (Into.Value(Args) && ...); };
		return Socket.EmitArguments(Name, Event, Write, SendFlags);
	}
	template <typename T> EVaRestSendResult EmitArgs(FStringView Event, const T& Args) const
	{
		static_assert(VaRest::Json::bHasScriptStruct<T>, "EmitArgs requires a USTRUCT with converted positional fields");
		return Socket.EmitStructArguments(Name, Event, VaRest::Json::GetScriptStruct<T>(), &Args, SendFlags);
	}
	FVaRestSioEmitter Volatile() const;
	template <typename... A> [[nodiscard]] FVaRestSioEmitWithAck EmitWithAck(FStringView Event, A&&... Args) const;
private:
	friend class FVaRestSioSocket;
	friend class FVaRestSioEmitter;
	TFunction<void(const FVaRestSocketMessage&)> EventHandler(TFunction<void(const FVaRestSioEvent&)> Handler) const;
	FVaRestSioNamespace(FVaRestSioSocket InSocket, FString InName) : Socket(MoveTemp(InSocket)), Name(MoveTemp(InName)) {}
	FVaRestSioSocket Socket;
	FString Name;
	EVaRestSend SendFlags = EVaRestSend::Default;
};

/** Any-thread send view; only the volatile emit operations are exposed. */
class VARESTSOCKETIO_API FVaRestSioEmitter
{
public:
	bool IsValid() const { return Namespace.Socket.IsValid(); }
	template <typename... A> EVaRestSendResult Emit(FStringView Event, A&&... Args) const { return Namespace.Emit(Event, Forward<A>(Args)...); }
	template <typename T> EVaRestSendResult EmitArgs(FStringView Event, const T& Args) const { return Namespace.EmitArgs(Event, Args); }
private:
	friend class FVaRestSioSocket;
	friend class FVaRestSioNamespace;
	explicit FVaRestSioEmitter(FVaRestSioNamespace Value) : Namespace(MoveTemp(Value)) { Namespace.SendFlags = EVaRestSend::Volatile; }
	FVaRestSioNamespace Namespace;
};

inline FVaRestSioEmitter FVaRestSioNamespace::Volatile() const { return FVaRestSioEmitter(*this); }
inline FVaRestSioEmitter FVaRestSioSocket::Volatile() const { return FVaRestSioEmitter(FVaRestSioNamespace(*this, {})); }

template <typename... A> EVaRestSendResult FVaRestSioSocket::Emit(FStringView Event, A&&... Args)
{ return FVaRestSioNamespace(*this, {}).Emit(Event, Forward<A>(Args)...); }
template <typename T> EVaRestSendResult FVaRestSioSocket::EmitArgs(FStringView Event, const T& Args)
{ return FVaRestSioNamespace(*this, {}).EmitArgs(Event, Args); }

class VARESTSOCKETIO_API FVaRestSio
{
public:
	static FVaRestSioSocket Make(const UObject* WorldContext, FStringView Url);
	static FVaRestSioSocket Make(const UObject* WorldContext, FName Name, FStringView Url);
	/** Explicit Hub also supports the editor fallback obtained through UVaRestSocketHub::GetForEditor(). */
	static FVaRestSioSocket Make(UVaRestSocketHub* Hub, FName Name, FStringView Url);
	/** Explicit namespace overrides the URL path's code-first namespace selection. */
	static FVaRestSioSocket Make(UVaRestSocketHub* Hub, FName Name, FStringView Url, FStringView Namespace);
	static FVaRestSioSocket Find(UVaRestSocketHub* Hub, FName Name);
	static FVaRestSioSocket Socket(const UObject* WorldContext, UVaRestApi* Api, FName Entry, FVaRestError& Error);
	static FVaRestSioSocket Socket(UVaRestSocketHub* Hub, UVaRestApi* Api, FName Entry, FVaRestError& Error);
private:
	static bool InitializeOnGT(FVaRestSocket& Socket, FVaRestSioStageOptions Options, const UVaRestSioSettings& Settings);
	static FVaRestSioSocket MakeOnGT(UVaRestSocketHub* Hub, FName Name, FStringView Url, TOptional<FString> Namespace);
};

#include "VaRestSioEmitWithAck.h"
#include "VaRestSioTyped.h"
