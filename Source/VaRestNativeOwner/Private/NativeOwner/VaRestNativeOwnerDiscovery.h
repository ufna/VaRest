// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestNativeOwnerCatalogue.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
namespace VaRestNativeOwnerPrivate::Discovery1
{
    // Read facts in the actual same-image Q4 scratch, never an authority packet.
    // NormalOwnerRecord names the sole fully qualified ordinary ABI2 normal
    // owner's coherent Bootstrap in Scratch.Candidates under genuine O. This
    // includes known historical72 for ONLY the original qualified ordinary
    // query/poll/retire treatment. No historical tail/private reuse is allowed;
    // Known historical Profile1/12 whole216 remains an ordinary-only blocker.
    // Q4 alone requires actual qualified Profile2/140 for current private reuse,
    // blocks historical216 before S and otherwise
    // applies its independently permitted old-source policy or blocks.
    // NormalOwnerRecord==UINT32_MAX means no retained route. Only OK may be
    // consumed; failed CurrentRecord is at most a historical locator. The actual
    // self getter's transient busy bit is retained as raw read data, not an
    // admission output; Q4 freshly revalidates/synthesizes its own result.
    // No pointer into scratch may escape its real owning transaction/frame.
    struct FQDiscovery1
    {
        uint64_t SelfBias, SelfGeneration;
        uint32_t CurrentRecord, NormalOwnerRecord, Images, Candidates;
    };
    static_assert(sizeof(FQDiscovery1)==32 && alignof(FQDiscovery1)==8);
    static_assert(std::is_trivial_v<FQDiscovery1> && std::is_standard_layout_v<FQDiscovery1>);
    // ZERO callers. Actual Q4 already owns the legitimate control/activation
    // interval and its SOLE scratch. Native ActivationDepth1 precedes this call;
    // it is NOT set/bound here, and neither bits nor these refs grant authority.
    // Full immutable origin/history/receipt/loader/RO/O/H/return premises are
    // required before invoking this complete dormant implementation.
    // The ONE preliminary namespace census does not certify BASE: actual Q4
    // must prove BASE/revalidate own state before S/ownership or same-O reuse.
    [[gnu::visibility("hidden")]] VrStatus LoadCurrentAndCensus1(
        Deployment1::FQScratchStorage1&, Deployment1::FQPhase1&,
        Deployment1::FQReadLedger1&, FQDiscovery1&);
}
#endif
