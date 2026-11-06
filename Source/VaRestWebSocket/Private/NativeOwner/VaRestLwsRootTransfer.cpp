// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestLwsRootTransfer.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
#include "../VaRestLwsService.h"
#include "VaRestNativeOwnerPump.h"
#include "Interfaces/ISslCertificateManager.h"
#include "CoreGlobals.h"
#include "HAL/PlatformTLS.h"

namespace VaRest::WebSocket::Lws
{
namespace EngineRootsDetail1
{
    using namespace VaRest::WebSocket::NumericConnectorC1;
    static bool Span(const void* P, uint64_t N, uint32_t A)
    { return P && reinterpret_cast<uintptr_t>(P) % A == 0 && N <= UINTPTR_MAX - reinterpret_cast<uintptr_t>(P); }
    static bool Separate(const void* A, uint64_t N, const void* B, uint64_t M)
    {
        const uintptr_t X = reinterpret_cast<uintptr_t>(A), Y = reinterpret_cast<uintptr_t>(B);
        return N <= UINTPTR_MAX - X && M <= UINTPTR_MAX - Y && (X + N <= Y || Y + M <= X);
    }
    static bool External(const FResourceBinding1* R, const void* P, uint64_t N, uint32_t A)
    { return Span(P, N, A) && Separate(R->Journal, 9248, P, N); }
    static bool Known(const FResourceCell1& C)
    { return C.LocalState == ResourceObtainedPending1 || C.LocalState == ResourceOwnedAck1; }
    static int32_t BorrowRefusal(const VrLwsCustody1* V)
    {
        return V->Control->Fault & 2u ? VR_LWS_RELEASE_UNCERTAIN1 : VR_LWS_PROTOCOL_FAULT1;
    }
    static int32_t Result(const VrLwsCustody1* V, ssl_ctx_st* Ctx,
        uint32_t Cell, VrLwsResult1* Out, int32_t Disposition)
    {
        const auto* R = static_cast<const FResourceBinding1*>(V->ResourceBinding);
        uint32_t Pending = 0;
        for (uint32_t I = 12; I < 140; ++I)
            if (R->Journal->Cells[I].LocalState != ResourceEmpty1) ++Pending;
        if (V->Control->Fault & 2u) Disposition = VR_LWS_RELEASE_UNCERTAIN1;
        *Out = {sizeof(*Out), VR_LWS_CUSTODY_VERSION1, static_cast<uint32_t>(Disposition),
            Cell, Ctx, Pending, V->Control->Fault};
        return Disposition; // Pool count/Fault are diagnostics, never closure proof.
    }
}

[[gnu::used]] int32_t FTrackedEngineRoots1::Install(lws_vhost* Vhost, ssl_ctx_st* Ctx,
    const VrLwsCustody1* V, const VrLwsRowRef1* Ref, VrLwsPhaseBudget1* Phase,
    uint32_t* Stage, VrLwsResult1* Out)
{
    using namespace EngineRootsDetail1;
    // Genuine accessible exclusive source spans/lifetimes precede these checks.
    uint32_t State = UINT32_MAX;
    VrLwsHookOutcome1 Observed{VR_LWS_HOOK_PROTOCOL_FAULT1, 0, 0, 0};
    varest_ws_c2l_observe_row1(V, Ref, &State, &Observed);
    if (Observed.Disposition != VR_LWS_HOOK_COMPLETED1 || Observed.NativeStatus
        || Observed.NativeCalls || Observed.Reserved) return VR_LWS_PROTOCOL_FAULT1;
    // observe_row validated the ORIGINAL View and actual original typed Binding.
    const auto* R = static_cast<const FResourceBinding1*>(V->ResourceBinding);
    if (!External(R, Phase, sizeof(*Phase), 4) || !External(R, Stage, sizeof(*Stage), 4)
        || !External(R, Out, sizeof(*Out), 8)
        || !Separate(Ref, sizeof(*Ref), Phase, sizeof(*Phase))
        || !Separate(Ref, sizeof(*Ref), Stage, sizeof(*Stage))
        || !Separate(Ref, sizeof(*Ref), Out, sizeof(*Out))
        || !Separate(Phase, sizeof(*Phase), Stage, sizeof(*Stage))
        || !Separate(Phase, sizeof(*Phase), Out, sizeof(*Out))
        || !Separate(Stage, sizeof(*Stage), Out, sizeof(*Out))) return VR_LWS_PROTOCOL_FAULT1;
    Result(V, Ctx, Ref->Cell, Out, VR_LWS_PROTOCOL_FAULT1);
    if (!Vhost || !Ctx || *Stage != VR_LWS_ENGINE_ROOTS_NEVER1
        || Phase->Phase != VR_LWS_PHASE_STARTUP1 || Phase->Reserved || Phase->Used > 1536
        || Phase->Remaining != 1536 - Phase->Used || V->Control->Fault)
        return static_cast<int32_t>(Out->Disposition);
    const FResourceCell1& Context = R->Journal->Cells[Ref->Cell];
    if (!Known(Context) || Context.Kind != 8 || Context.Subkind != 2
        || Context.NativeValue != reinterpret_cast<uintptr_t>(Ctx)) return VR_LWS_PROTOCOL_FAULT1;
    uint32_t Parents = 0;
    for (uint32_t I = 12; I < 140; ++I)
    {
        const FResourceCell1& P = R->Journal->Cells[I];
        if (Known(P) && P.Kind == 6 && P.Subkind == 1 && P.NativeSerial == Context.ParentSerial
            && P.NativeValue == reinterpret_cast<uintptr_t>(Vhost)) ++Parents;
    }
    if (Parents != 1 || !V->Control->RawContext || !V->Control->FullCreateReturned
        || V->Control->FullDestroyReturned) return VR_LWS_PROTOCOL_FAULT1;
    if (RequireNativeOwnerControlThread(TEXT("tracked Engine roots creating call")) != VR_STATUS_OK)
        return VR_LWS_PROTOCOL_FAULT1;
    // Actual C source validated Vhost and its exact containing root-stage field.
    // This roundtrip is the ACTUAL original FService, protected by its creating
    // caller's strong lifetime; pointer/phase checks do not supply that premise.
    auto* Service = static_cast<FService*>(lws_get_vhost_user(Vhost));
    if (!Service || static_cast<const void*>(Service->NativeCreating.Resources) != static_cast<const void*>(R->Journal)
        || Service->NativeCreating.Phase != FService::ENativeEntryPhase1::Attached
        || !Service->NativeCreating.InCreate || Service->NativeCreating.InEntry || Service->NativeCreating.InCleanup
        || Service->NativeChildSnapshot || Service->Thread || Service->bAccepting.load()
        || Service->bShuttingDown.load() || !Service->Certificates || !Service->SslManager)
        return VR_LWS_PROTOCOL_FAULT1;
    const VrLwsBudgetRef1 Budget{nullptr, Phase};
    VrLwsHookOutcome1 Debit{VR_LWS_HOOK_PROTOCOL_FAULT1, 0, 0, 0};
    varest_ws_c2l_debit_named1(V, Ref, &Budget, VR_LWS_NAMED_ENGINE_ROOTS1, &Debit);
    if (Debit.NativeStatus == 19) V->Control->Fault |= 2u; // Actual hook observation ONLY.
    if (Debit.Disposition != VR_LWS_HOOK_COMPLETED1 || Debit.NativeStatus || Debit.NativeCalls || Debit.Reserved)
        return Result(V, Ctx, Ref->Cell, Out, V->Control->Fault & 2u ? VR_LWS_RELEASE_UNCERTAIN1 : VR_LWS_FAILED_WITH_OUTSTANDING1);
    // Exactly ONE existing Engine API call. No per-root enumeration/visited or
    // all-roots-success receipt is invented; inner refs/work belong to CTX H.
    *Stage = VR_LWS_ENGINE_ROOTS_ENTERED1;
    Service->Certificates->AddCertificatesToSslContext(Ctx);
    *Stage = VR_LWS_ENGINE_ROOTS_ENGINE_RETURNED1;
    // C caller, and ONLY after this WHOLE bridge returns, advances 2 to 3.
    // Direct ExtraTrustedPem BIO/X509/store work is a separate tracked C3 step.
    return Result(V, Ctx, Ref->Cell, Out, VR_LWS_KNOWN_SUCCESS1);
}

[[gnu::used]] int32_t FTrackedEngineRoots1::BorrowExtraTrustedPem(lws_vhost* Vhost, ssl_ctx_st* Ctx,
    const VrLwsCustody1* V, const VrLwsRowRef1* Ref, const uint8_t** Data,
    uint32_t* Bytes, VrLwsHookOutcome1* Out)
{
    using namespace EngineRootsDetail1;
    // Actual accessible/exclusive source spans and protected creating lifetime
    // precede this helper. No returned pointer/shape appoints those premises.
    if (!Span(V, sizeof(*V), 8)) return VR_LWS_PROTOCOL_FAULT1;
    const auto* R = static_cast<const FResourceBinding1*>(V->ResourceBinding);
    if (!Span(R, sizeof(*R), 8) || !Span(R->Journal, 9248, 8)) return VR_LWS_PROTOCOL_FAULT1;
    const uintptr_t Journal = reinterpret_cast<uintptr_t>(R->Journal);
    if (reinterpret_cast<uintptr_t>(R) != Journal + 9072 || reinterpret_cast<uintptr_t>(V) != Journal + 9232
        || reinterpret_cast<uintptr_t>(V->Control) != Journal + 9168) return VR_LWS_PROTOCOL_FAULT1;
    // From this actual original View onward, an existing observed19 dominates
    // every refusal; it never becomes a fabricated NativeStatus in HookOut.
    if (!ResourceBindingValid1(R) || V->Control->Reserved0 || V->Control->Reserved1) return BorrowRefusal(V);
    uint32_t State = UINT32_MAX;
    VrLwsHookOutcome1 Observed{VR_LWS_HOOK_PROTOCOL_FAULT1, 0, 0, 0};
    varest_ws_c2l_observe_row1(V, Ref, &State, &Observed);
    if (Observed.Disposition != VR_LWS_HOOK_COMPLETED1 || Observed.NativeStatus
        || Observed.NativeCalls || Observed.Reserved) return BorrowRefusal(V);
    if (!External(R, Data, sizeof(*Data), 8) || !External(R, Bytes, sizeof(*Bytes), 4)
        || !External(R, Out, sizeof(*Out), 4)
        || !Separate(Ref, sizeof(*Ref), Data, sizeof(*Data))
        || !Separate(Ref, sizeof(*Ref), Bytes, sizeof(*Bytes))
        || !Separate(Ref, sizeof(*Ref), Out, sizeof(*Out))
        || !Separate(Data, sizeof(*Data), Bytes, sizeof(*Bytes))
        || !Separate(Data, sizeof(*Data), Out, sizeof(*Out))
        || !Separate(Bytes, sizeof(*Bytes), Out, sizeof(*Out))) return BorrowRefusal(V);
    if (!Vhost || !Ctx || !V->Control->RawContext || !V->Control->FullCreateReturned
        || V->Control->FullDestroyReturned) return BorrowRefusal(V);
    const FResourceCell1& Context = R->Journal->Cells[Ref->Cell];
    if (!Known(Context) || Context.Kind != 8 || Context.Subkind != 2
        || Context.NativeValue != reinterpret_cast<uintptr_t>(Ctx)) return BorrowRefusal(V);
    uint32_t Parents = 0;
    for (uint32_t I = 12; I < 140; ++I)
    {
        const FResourceCell1& Parent = R->Journal->Cells[I];
        if (Known(Parent) && Parent.SourceSerial && Parent.Kind == 6 && Parent.Subkind == 1
            && Parent.NativeSerial == Context.ParentSerial
            && Parent.NativeValue == reinterpret_cast<uintptr_t>(Vhost)) ++Parents;
    }
    if (Parents != 1 || !GIsGameThreadIdInitialized
        || FPlatformTLS::GetCurrentThreadId() != GGameThreadId || !IsInGameThread())
        return BorrowRefusal(V); // No logging callback on this pure refusal path.
    auto* Service = static_cast<FService*>(lws_get_vhost_user(Vhost));
    if (!Span(Service, sizeof(*Service), alignof(FService))
        || static_cast<const void*>(Service->NativeCreating.Resources) != static_cast<const void*>(R->Journal)
        || Service->NativeCreating.Phase != FService::ENativeEntryPhase1::Attached
        || !Service->NativeCreating.InCreate || Service->NativeCreating.InEntry || Service->NativeCreating.InCleanup
        || Service->NativeChildSnapshot || Service->Thread || Service->bAccepting.load()
        || Service->bShuttingDown.load() || !Service->Certificates || !Service->SslManager)
        return BorrowRefusal(V);
    // Protect the actual typed TArray/Service metadata as well as its backing.
    // Outputs may be the distinct intended fields inside the genuine vhost.
    if (!Separate(Service, sizeof(*Service), Data, sizeof(*Data))
        || !Separate(Service, sizeof(*Service), Bytes, sizeof(*Bytes))
        || !Separate(Service, sizeof(*Service), Out, sizeof(*Out))) return BorrowRefusal(V);
    const int32 Count = Service->Options.ExtraTrustedPem.Num();
    const uint8_t* Pem = Service->Options.ExtraTrustedPem.GetData();
    if (Count < 0) return BorrowRefusal(V); // No known backing extent: no output stores.
    if (Count > 0)
    {
        const uint64_t Length = static_cast<uint64_t>(Count);
        if (!External(R, Pem, Length, 1) || !Separate(Service, sizeof(*Service), Pem, Length)
            || !Separate(Pem, Length, Ref, sizeof(*Ref))
            || !Separate(Pem, Length, Data, sizeof(*Data))
            || !Separate(Pem, Length, Bytes, sizeof(*Bytes))
            || !Separate(Pem, Length, Out, sizeof(*Out))) return BorrowRefusal(V);
    }
    // Every known output/input/holder/actual nonempty-backing alias is excluded
    // BEFORE the first caller-output store, including oversized-input refusal.
    *Data = nullptr; *Bytes = 0; *Out = {VR_LWS_HOOK_LOCAL_INVALID1, 0, 0, 0};
    if (V->Control->Fault)
    { Out->Disposition = VR_LWS_HOOK_PROTOCOL_FAULT1; return BorrowRefusal(V); }
    if (Count > 65536) return VR_LWS_PROTOCOL_FAULT1;
    if (Count) { *Data = Pem; *Bytes = static_cast<uint32_t>(Count); }
    Out->Disposition = VR_LWS_HOOK_COMPLETED1;
    return VR_LWS_KNOWN_SUCCESS1;
}
}

extern "C" [[gnu::used, gnu::visibility("hidden")]] int32_t varest_ws_c2l_install_engine_roots1(
    lws_vhost* Vhost, ssl_ctx_st* Ctx, const VrLwsCustody1* View,
    const VrLwsRowRef1* Row, VrLwsPhaseBudget1* Phase, uint32_t* Stage, VrLwsResult1* Out)
{
    return VaRest::WebSocket::Lws::FTrackedEngineRoots1::Install(Vhost, Ctx, View, Row, Phase, Stage, Out);
}

extern "C" [[gnu::used, gnu::visibility("hidden")]] int32_t varest_ws_c2l_borrow_extra_trusted_pem1(
    lws_vhost* Vhost, ssl_ctx_st* Ctx, const VrLwsCustody1* View, const VrLwsRowRef1* Row,
    const uint8_t** Data, uint32_t* Bytes, VrLwsHookOutcome1* Out)
{
    return VaRest::WebSocket::Lws::FTrackedEngineRoots1::BorrowExtraTrustedPem(Vhost, Ctx, View, Row, Data, Bytes, Out);
}
#endif
