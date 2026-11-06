// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once
#include "Connector/VaRestNumericConnectorNative.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
namespace VaRest::WebSocket::Lws
{
namespace C2ResourcesC1 = VaRest::WebSocket::NumericConnectorC1;
class FService;
// ZERO actual resource-creating callers. The dormant FService metadata graph
// owns this until every real resource/callback/receipt and code return retires.
// The destructor performs no acknowledgement, close, reset or native operation.
class FNativeOwnerResources1 final
{
    friend class FService;
    FNativeOwnerResources1(void* owner, uint64_t ownerEpoch, uint64_t serviceEpoch,
        C2ResourcesC1::FResourceEntryAddress1 reserve, C2ResourcesC1::FResourceEntryAddress1 obtained,
        C2ResourcesC1::FResourceEntryAddress1 notObtained, C2ResourcesC1::FResourceEntryAddress1 closed,
        C2ResourcesC1::FResourceEntryAddress1 uncertain);
    ~FNativeOwnerResources1() = default;
    FNativeOwnerResources1(const FNativeOwnerResources1&) = delete;
    FNativeOwnerResources1& operator=(const FNativeOwnerResources1&) = delete;
    FNativeOwnerResources1(FNativeOwnerResources1&&) = delete;
    FNativeOwnerResources1& operator=(FNativeOwnerResources1&&) = delete;
    const C2ResourcesC1::FResourceBinding1* GetBinding1() const;
    C2ResourcesC1::FStorageJournal1* GetStorage1();
    // ONLY after FULL C1 return in the actual pre-child/no-child/known-join
    // creating frame, never a GT frame callback while child may mutate Journal.
    // I2 AfterKnownCreatingResourcesClosed/value retirement is later still: every
    // known C1/C2-L disposition, callback/full return and native ACK must finish,
    // with no pending/outstanding/fault/uncertainty. No pointer/phase proves it.
    C2ResourcesC1::FResourceProgress1 ReconcileAfterC1ReturnOnCreatingThread(VrLwsPhaseBudget1* actualCreatingPhase);
    // Same external startup/final phase gas spans every pass; no per-pass renewal.
    C2ResourcesC1::FResourceJournal1 Journal;
    C2ResourcesC1::FStorageJournal1 Storage;
    const C2ResourcesC1::FResourceBinding1 Binding;
    VrLwsSourceControl1 Control;
    const VrLwsCustody1 View;
};
}
#endif
