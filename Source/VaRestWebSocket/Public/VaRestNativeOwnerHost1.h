// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreTypes.h"
#include "Modules/ModuleInterface.h"

// Actual requested WS-operation progress, never a native ownership/H/exit
// certificate. Refused means this call was refused; existing duty may remain.
enum class EVaRestNativeOwnerHostOutcome1 : uint32
{
    Complete = 0, Pending = 1, Refused = 2, Retained = 3
};
struct FVaRestNativeOwnerHostStep1
{
    uint32 Size, Version, Outcome;
    int32 ActualStatus;
};
static_assert(sizeof(FVaRestNativeOwnerHostStep1) == 16);
static_assert(alignof(FVaRestNativeOwnerHostStep1) == alignof(uint32));

// Caller already protects the ACTUAL normally loaded WS module, Core/provider,
// physical initialized GT, source H and every code/return/pending lifetime.
// A module/interface/shared pointer supplies none of those prerequisites.
// ActualStatus preserves the actual WS control result (including prior actual19
// in its source journal), or the stated local refusal, NOT a new native result.
// Complete covers this WS session operation only: not producer O, remaining
// native worker/lease/process retirement, safe module unload or process exit.
// Ordinary C++ INTERFACE REVISION 2: the first three virtuals keep their order;
// slot four is valid ONLY with the genuinely qualified exact revision-2 module.
// Result POD format stays Version1. No cast/name/ready bit negotiates this ABI.
// BEFORE Start, actual Host control flow must already commit one direct call
// after each completed GT frame, outside delegate dispatch, with every finish,
// retained-duty exit and FULL callback/Leave/return lifetime protected. Calling
// Pump once cannot establish that interval; Source H/Host integration is CLOSED.
// Terminal frame results require the Host to cease future dispatch and retain
// the real result/graph/code/lease. A later local refusal never discharges it.
// No default selection/Host5/Game activation follows from this declaration.
class VARESTWEBSOCKET_API IVaRestNativeOwnerHost1 : public IModuleInterface
{
public:
    virtual FVaRestNativeOwnerHostStep1 StartNativeOwnerSession1() = 0;
    virtual FVaRestNativeOwnerHostStep1 FinishNativeOwnerSession1() = 0;
    virtual FVaRestNativeOwnerHostStep1 AdvanceNativeOwnerFinish1() = 0;
    virtual FVaRestNativeOwnerHostStep1 PumpNativeOwnerSession1() = 0;
};
