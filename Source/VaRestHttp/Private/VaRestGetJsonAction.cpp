// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestGetJsonAction.h"
#include "VaRestDynamicResult.h"

void UVaRestGetJsonAction::BroadcastSuccessOnGT(const FVaRestDynamicResult& Result)
{
	check(IsInGameThread());
	const FVaRestJson Json=Result.GetJson(); // exactly one native plain-document take for all listeners
	OnSuccessJson.Broadcast(Json,GetTerminalResponse());
	if (CanDeliverOnGT()) Super::BroadcastSuccessOnGT(Result);
}
