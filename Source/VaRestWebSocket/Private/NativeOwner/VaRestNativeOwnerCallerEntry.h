// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once
#include "VaRestNativeOwnerEntryGate.h"
#include "VaRestNativeOwnerSourceSupplier.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
namespace VaRest::WebSocket::Lws
{
    // A real single initiating stack's records, never a permission packet. The
    // qualified host retains WS/Core/provider/graph/return lifetime, including
    // every retained outcome. A destructor NEVER closes, resets or acknowledges.
    // NativeOutcome/status remain the exact actual return. The fixed I3-A
    // FService handoff projects only the genuine returned current14/216 A,
    // during continuous O/source protection, after selected-B callables clear
    // and before E/fd/final Core disposition. No installable binding escapes.
    class [[gnu::visibility("hidden")]] FCallerEntryTransaction final
    {
        friend class FTransport;
        friend class FService;
        friend class FNativeSourceSupplier;
        FCallerEntryTransaction();
        FCallerEntryTransaction(const FCallerEntryTransaction&)=delete;
        FCallerEntryTransaction& operator=(const FCallerEntryTransaction&)=delete;
        FCallerEntryTransaction(FCallerEntryTransaction&&)=delete;
        FCallerEntryTransaction& operator=(FCallerEntryTransaction&&)=delete;
        ~FCallerEntryTransaction()=default;
        VrStatus RunForService(FService&);
        FCallerReadLedger1 Files;
        FCallerEJournal1 Entry;
        FCallerPrivateBinding1 SelectedEntry;
        VrBootstrap1 NativeOutcome;
        FNativeOwnerEntryGate1 Gate;
        uint32 Inspections, Phase;
        VrStatus NativeStatus, DispositionStatus;
    };
    static_assert(sizeof(FCallerEntryTransaction)==384 && alignof(FCallerEntryTransaction)==8);
}
#endif
