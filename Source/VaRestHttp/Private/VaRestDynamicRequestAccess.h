// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestDynamicRequest.h"
#include "VaRestDynamicClosure.h"

namespace VaRest::Http
{
/** Internal preparation only; no private spec exposure or public arbitrary handler replacement. */
struct FDynamicRequestAccess
{
	/** An internal factory/action owns this builder; deliberate refusal/cancellation is not a missing public Send. */
	static void DiscardOnGT(TVaRestRequest<FVaRestDynamicResult>& Request)
	{
		check(IsInGameThread());
		if (Request.Spec) { Request.Spec->bSent=true; Request.Spec.Reset(); }
	}
	static void FailOnGT(TVaRestRequest<FVaRestDynamicResult>& Request,FString Error)
	{
		check(IsInGameThread()); if (Request.Spec) Request.Spec->Fail(MoveTemp(Error));
	}
	static FVaRestJsonConvertOptions EncodeOptionsOnGT(const TVaRestRequest<FVaRestDynamicResult>& Request)
	{
		check(IsInGameThread()); FVaRestJsonConvertOptions Result;
		if (Request.Spec) { Result.Naming=Request.Spec->Naming; Result.Rules=Request.Spec->JsonRules; }
		return Result;
	}
	static void AppendHeaderOnGT(TVaRestRequest<FVaRestDynamicResult>& Request,FStringView Name,FStringView Value)
	{
		check(IsInGameThread()); if (!Request.Spec) return;
		if (!IsValidHeaderName(Name) || !IsValidHeaderValue(Value)) { Request.Spec->Fail(TEXT("invalid ordered request header")); return; }
		Request.Spec->bPreserveOwnHeaderLines=true; Request.Spec->Headers.Add(Name,Value);
	}
	static bool HasHeaderOnGT(const TVaRestRequest<FVaRestDynamicResult>& Request,FStringView Name)
	{
		check(IsInGameThread());return Request.Spec && Request.Spec->Headers.Contains(Name);
	}
	static void BeginNativeDecodeOnGT(const FVaRestExchangeRef& Exchange,FFinish&& Finish);
	static bool ValidateOnGT(TVaRestRequest<FVaRestDynamicResult>& Request,FString& Error)
	{
		check(IsInGameThread());
		if (!Request.Spec) { Error=TEXT("DynamicUnavailable"); return false; }
		return Request.Handler().ValidateConfiguration(Error);
	}
	static bool SetActionLifetimeOnGT(TVaRestRequest<FVaRestDynamicResult>& Request,TUniqueFunction<void()>&& Cleanup)
	{
		check(IsInGameThread());
		return Request.Spec && Request.Handler().SetActionLifetimeOnGT(MoveTemp(Cleanup));
	}
	static TWeakPtr<FDynamicTypedState,ESPMode::ThreadSafe> GetTypesOnGT(TVaRestRequest<FVaRestDynamicResult>& Request)
	{
		check(IsInGameThread());
		return Request.Spec ? Request.Handler().GetTypesOnGT() : TWeakPtr<FDynamicTypedState,ESPMode::ThreadSafe>();
	}
	static bool SetPreparationOnGT(TVaRestRequest<FVaRestDynamicResult>& Request,TUniqueFunction<void(FDynamicPrepareToken&)>&& Callback)
	{
		check(IsInGameThread());
		return Request.Spec && Request.Handler().SetPreparationOnGT(MoveTemp(Callback));
	}
};
}
