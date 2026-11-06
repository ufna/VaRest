// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestDynamicResult.h"
#include "VaRestResultHandler.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/StrongObjectPtr.h"

namespace VaRest::Http
{
class FDynamicTypedState;
struct FDynamicTypeRuntimeData;

/** Stack-only capability for one synchronous internal preparation. No public shell can obtain this token. */
class FDynamicPrepareToken final
{
public:
	FDynamicPrepareToken(const FDynamicPrepareToken&) = delete;
	FDynamicPrepareToken& operator=(const FDynamicPrepareToken&) = delete;
	bool Take(const FProperty* Destination, void* Address, bool bError = false);
	bool ConfirmPrepared();
	bool IsError() const;
private:
	friend class FDynamicTypedState;
	explicit FDynamicPrepareToken(FDynamicTypedState& InState) : State(InState) {}
	FDynamicTypedState& State;
};

/** All strong ownership of this class is GT-only and lives in the reserved frame payload, never handler worker data. */
class FDynamicTypedState final : public TSharedFromThis<FDynamicTypedState, ESPMode::ThreadSafe>
{
public:
	static TSharedPtr<FDynamicTypedState, ESPMode::ThreadSafe> CreateOnGT(const FVaRestDynamicResultConfig& Config, FString& Error,FString* Code=nullptr,
		TSharedPtr<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe> RulesOverride = {});
	~FDynamicTypedState();
	FName ResultNameOnGT() const;
	FName ErrorNameOnGT() const;
	bool NeedsResponseJsonOnGT() const;
	bool HasTypedResponseOnGT() const;
	bool HasTypedErrorOnGT() const;
	bool ValidateActivationOnGT(FString& Error) const;
	TSharedPtr<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe> RulesOnGT() const { check(IsInGameThread()); return Rules; }
	/** Post-Completed availability only; never changes the already accepted native outcome. */
	bool IsCurrentForDeliveryOnGT() const { check(IsInGameThread()); return bFinalized && CurrentOnGT(); }
	void FinalizeOnGT(FFinish& Finish, const FVaRestJson& Json, FVaRestDynamicTypeInfo& Response, FVaRestDynamicTypeInfo& Error,
		const FVaRestDynamicLimits& Limits, FVaRestDynamicBudgetLease& Lease);
	void DiscardOnGT();
	void ValidateTerminalOnGT(FFinish& Finish,FVaRestDynamicTypeInfo& Response,FVaRestDynamicTypeInfo& Error) const;
	void InvalidateOnGT();
	bool SetPreparationOnGT(TUniqueFunction<void(FDynamicPrepareToken&)>&& Callback);
private:
	friend class FDynamicPrepareToken;
	friend class FDynamicTypeRuntime;
	struct FCarrier
	{
		FVaRestTypeRef Type;
		TStrongObjectPtr<const UPropertyBag> BagType;
		FInstancedStruct Struct;
		FInstancedPropertyBag Bag;
		bool bReady = false;
		bool bTyped = false;
#if WITH_DEV_AUTOMATION_TESTS
		bool bCounted = false;
#endif
		void Reset();
	};
	bool CurrentOnGT() const;
	bool MaterializeOnGT(FCarrier& Carrier, const FVaRestJson& Json, const FVaRestJsonConvertOptions& Options, FVaRestJsonDecodeReport& Report);
	bool TakeOnGT(const FProperty* Destination, void* Address, bool bError);
	FCarrier ResponseCarrier, ErrorCarrier;
	TArray<TStrongObjectPtr<const UObject>> Pins;
	TWeakObjectPtr<const UObject> RuleOwner;
	TSharedPtr<const FVaRestJsonRuleSnapshot, ESPMode::ThreadSafe> Rules;
	TUniqueFunction<void(FDynamicPrepareToken&)> Preparation;
	uint64 Schema = 0, OwnerRules = 0, DefaultRules = 0, NativeNames = 0;
	bool bStale = false, bPreparing = false, bFinalized = false, bHadRuleOwner = false;
	bool bPreparationTaken = false, bPreparationConfirmed = false;
	bool bPreparationBound = false;
	bool bHadPrimaryError = false;
	bool bPositionalResponse = false;
};

/** Module lifetime, fixed256 weak tracking entries and weak64 LRU bag descriptors. No worker accesses this object. */
class FDynamicTypeRuntime final
{
public:
	static TUniquePtr<FDynamicTypeRuntime> CreateOnGT();
	~FDynamicTypeRuntime();
	void StopOnGT();
private:
	TUniquePtr<FDynamicTypeRuntimeData> Impl;
};
}
