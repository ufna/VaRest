// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "VaRestSioSocket.h"

namespace VaRest::Sio { struct FAckControl; }
namespace VaRest::Sio::Typed
{
template <typename F> TFunction<void(const FVaRestSioEvent&)> Handler(FVaRestSocketWeak Socket, TFunction<bool()> OwnerAlive, F&& Function);
}

/** Native weak identity. Cancel/IsActive may run on any thread and never own an application callback. */
class VARESTSOCKETIO_API FVaRestSioAckHandle
{
public:
	FVaRestSioAckHandle() = default;
	bool IsActive() const;
	void Cancel() const;
private:
	friend class FVaRestSioEmitWithAck;
	TWeakPtr<VaRest::Sio::FAckControl, ESPMode::ThreadSafe> Control;
};

/** GT-only move-only builder. Arguments are encoded synchronously when the builder is created. */
class VARESTSOCKETIO_API FVaRestSioEmitWithAck
{
public:
	FVaRestSioEmitWithAck(FVaRestSioEmitWithAck&& Other);
	FVaRestSioEmitWithAck& operator=(FVaRestSioEmitWithAck&& Other);
	~FVaRestSioEmitWithAck();
	FVaRestSioEmitWithAck(const FVaRestSioEmitWithAck&) = delete;
	FVaRestSioEmitWithAck& operator=(const FVaRestSioEmitWithAck&) = delete;
	FVaRestSioEmitWithAck& Timeout(double Seconds);
	FVaRestSioEmitWithAck& ErrorFirst(bool bEnabled = true);
	FVaRestSioEmitWithAck& Retry(int32 Count);
	FVaRestSioEmitWithAck& Owner(const UObject* Owner);
	FVaRestSioEmitWithAck& OnSuccess(TFunction<void(const FVaRestSioEvent&)> Handler);
	template <typename F> FVaRestSioEmitWithAck& OnSuccess(F&& Function)
	{ return OnSuccess(VaRest::Sio::Typed::Handler(NativeSocketOnGT(), OwnerGuardOnGT(), Forward<F>(Function))); }
	FVaRestSioEmitWithAck& OnError(TFunction<void(const FVaRestError&)> Handler);
	/** GTK-only preparation after error-first interpretation and before the logical request's terminal trace. */
	FVaRestSioEmitWithAck& TransformReply(TFunction<FVaRestError(const FVaRestSioEvent&)> Transform);
	/** GTK-only internal scope cleanup, also on silent retirement; no application callbacks here. */
	FVaRestSioEmitWithAck& OnRetired(TFunction<void()> Cleanup);
	[[nodiscard]] FVaRestSioAckHandle Send();
	/** Explicitly retires an unsent prepared packet without a discarded-builder warning. GTK only. */
	void Discard();
private:
	friend class FVaRestSioSocket;
	FVaRestSioEmitWithAck(FVaRestSioSocket Socket, FString Namespace, FString Event, FVaRestSocketArgumentsWriter Arguments);
	struct FImpl;
	TUniquePtr<FImpl> Impl;
	FVaRestSocketWeak NativeSocketOnGT() const;
	TFunction<bool()> OwnerGuardOnGT() const;
};

template <typename... A> FVaRestSioEmitWithAck FVaRestSioNamespace::EmitWithAck(FStringView Event, A&&... Args) const
{
	check(IsInGameThread());
	auto Write = [&](auto& Writer, const FVaRestJsonConvertOptions& Options, FVaRestJsonError& Error, TFunctionRef<bool()> WithinBudget)
	{ VaRest::Sio::FArgumentWriter Into{Writer, Options, Error, WithinBudget}; return (Into.Value(Args) && ...); };
	return Socket.MakeAckBuilder(Name, Event, Write);
}
template <typename... A> FVaRestSioEmitWithAck FVaRestSioSocket::EmitWithAck(FStringView Event, A&&... Args)
{
	check(IsInGameThread());
	auto Write = [&](auto& Writer, const FVaRestJsonConvertOptions& Options, FVaRestJsonError& Error, TFunctionRef<bool()> WithinBudget)
	{ VaRest::Sio::FArgumentWriter Into{Writer, Options, Error, WithinBudget}; return (Into.Value(Args) && ...); };
	return MakeAckBuilder({}, Event, Write);
}
