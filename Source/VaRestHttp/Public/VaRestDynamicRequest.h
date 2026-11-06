// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestDynamicResult.h"
#include "VaRestResultHandler.h"

namespace VaRest::Http
{
template <> class TResultHandler<FVaRestDynamicResult>;
struct FDynamicHandlerData;
struct FDynamicRequestAccess;
class FDynamicPrepareToken;
class FDynamicTypedState;
}

#include "VaRestClient.h"

namespace VaRest::Http
{
/** Private-spec construction remains with FVaRestClient; this specialization preserves the fluent builder ABI. */
template <>
class VARESTHTTP_API TResultHandler<FVaRestDynamicResult> final : public IResultHandler
{
public:
	TResultHandler();
	explicit TResultHandler(FVaRestDynamicResultConfig&& Config);
	virtual ~TResultHandler() override;
	TUniqueFunction<void(const FVaRestDynamicResult&,const FVaRestResponse&)> OnSuccess;
	TUniqueFunction<void(const FVaRestError&)> OnError;
	TUniqueFunction<void(const TVaRestResult<FVaRestDynamicResult>&)> OnComplete;
	TUniqueFunction<void(const FVaRestProgress&)> OnProgress;
	virtual FName GetResultType() const override;
	virtual bool DecodesBody() const override;
	virtual bool DecodesJson() const override;
	virtual bool AppliesEnvelope() const override;
	virtual bool RequiresGameThreadDelivery() const override { return true; }
	virtual EResultCompletionPolicy GetCompletionPolicy() const override;
	virtual bool DeferCompletionOnGT(TUniqueFunction<void()>& Kernel) override;
	virtual int64 GetDynamicMaxInputBytes() const override;
	virtual void ConfigureParseOptions(FVaRestJsonParseOptions& Options) const override;
	virtual bool ValidateConfiguration(FString& Error) const override;
	virtual FString GetConfigurationFailureCode() const override;
	virtual bool PrepareDecodeOnGT(FFinish& Finish) override;
	virtual void Decode(FFinish& Finish) override;
	virtual void Finalize(FFinish& Finish) override;
	virtual void ValidateTerminalOnGT(FFinish& Finish) override;
	virtual void DiscardValue() override;
	virtual void Deliver(FFinish& Finish) override;
	virtual bool WantsProgress() const override { return bool(OnProgress); }
	virtual void Progress(const FVaRestProgress& Snapshot) override { if (OnProgress) OnProgress(Snapshot); }

private:
	friend struct FDynamicRequestAccess;
	bool SetPreparationOnGT(TUniqueFunction<void(FDynamicPrepareToken&)>&& Callback);
	bool SetActionLifetimeOnGT(TUniqueFunction<void()>&& Cleanup);
	TWeakPtr<FDynamicTypedState,ESPMode::ThreadSafe> GetTypesOnGT() const;
	TSharedPtr<FDynamicHandlerData,ESPMode::ThreadSafe> Data;
	TOptional<FVaRestDynamicResult> Value;
};
}
