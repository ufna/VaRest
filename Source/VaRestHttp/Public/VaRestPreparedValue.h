// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "VaRestDynamicResult.h"

class FProperty;
class FVaRestPreparedValue;

/** Internal stack-only transfer capability. It never owns or retains a destination property or address. */
class VARESTHTTP_API FVaRestPreparedValueToken final
{
public:
	FVaRestPreparedValueToken(const FVaRestPreparedValueToken&) = delete;
	FVaRestPreparedValueToken& operator=(const FVaRestPreparedValueToken&) = delete;
	bool Take(const FProperty* Destination, void* Address);
	bool ConfirmPrepared();
private:
	friend class FVaRestPreparedValue;
	explicit FVaRestPreparedValueToken(void* InToken) : Token(InToken) {}
	void* Token = nullptr;
};

/** Internal GTK-only adapter for an already parsed protocol value, using HTTP's qualified closure, carrier,
 * budget and module-stop control. This is not a result shell and must never be retained or destroyed by a worker.
 * A compiler-generated closed preparation copies once into engine-owned storage; carriers are gone on return.
 * Reflected worker conversion is never enabled by this adapter. */
class VARESTHTTP_API FVaRestPreparedValue final
{
public:
	static TUniquePtr<FVaRestPreparedValue> CreateOnGT(FVaRestDynamicResultConfig Config,
		TSharedPtr<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe> Rules, FVaRestError& Error);
	~FVaRestPreparedValue();
	/** An unused admitted plan pins the activation schema; each message gets its own bounded one-shot frame. */
	TUniquePtr<FVaRestPreparedValue> CloneOnGT(FVaRestError& Error) const;
	bool IsCurrentForActivationOnGT() const;
	FVaRestPreparedValue(const FVaRestPreparedValue&) = delete;
	FVaRestPreparedValue& operator=(const FVaRestPreparedValue&) = delete;
	/** One synchronous invocation; Prepare cannot retain its token or run user/latent Blueprint execution. */
	FVaRestError PrepareOnGT(const FVaRestJson& Value, const FVaRestJsonConvertOptions& Options,
		TFunctionRef<void(FVaRestPreparedValueToken&)> Prepare, TArray<FVaRestJsonError>& Warnings);
	bool IsCurrentForDeliveryOnGT() const;
	void RetireOnGT();
private:
	struct FImpl;
	explicit FVaRestPreparedValue(TSharedRef<FImpl> InState);
	TSharedPtr<FImpl> State;
};
