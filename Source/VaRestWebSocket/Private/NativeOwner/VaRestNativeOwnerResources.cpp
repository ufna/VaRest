// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeOwnerResources.h"
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
#include "VaRestNativeOwnerPrivateResource1.h"
#include <type_traits>

#define C2_BODY [[gnu::used, gnu::visibility("hidden")]]
namespace VaRest::WebSocket::NumericConnectorC1
{
namespace C2ResourceDetail
{
#define C2_LAYOUT(T, N, A) static_assert(sizeof(T) == N && alignof(T) == A); \
    static_assert(std::is_trivial_v<T> && std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>)
    C2_LAYOUT(FResourceTicket1, 40, 8); C2_LAYOUT(FResourceSpec1, 24, 8);
    C2_LAYOUT(FResourceActual1, 32, 8); C2_LAYOUT(FResourceCell1, 64, 8);
    C2_LAYOUT(FResourceJournalHeader1, 64, 8); C2_LAYOUT(FResourceJournal1, 9024, 8);
    C2_LAYOUT(FResourceBinding1, 96, 8); C2_LAYOUT(FHookOutcome1, 16, 4);
    C2_LAYOUT(FResourceProgress1, 32, 4);
    C2_LAYOUT(VrResourceTicket1, 40, 8); C2_LAYOUT(VrResourceSpec1, 24, 8);
    C2_LAYOUT(VrResourceActual1, 32, 8); C2_LAYOUT(VrResourceReply1, 64, 8);
#undef C2_LAYOUT
#define C2_OFFSET(T, F, N) static_assert(offsetof(T, F) == N)
    C2_OFFSET(FResourceTicket1, Size, 0); C2_OFFSET(FResourceTicket1, Version, 4);
    C2_OFFSET(FResourceTicket1, OwnerEpoch, 8); C2_OFFSET(FResourceTicket1, ServiceEpoch, 16);
    C2_OFFSET(FResourceTicket1, Serial, 24); C2_OFFSET(FResourceTicket1, Cell, 32);
    C2_OFFSET(FResourceTicket1, Kind, 36); C2_OFFSET(FResourceTicket1, Subkind, 38);
    C2_OFFSET(FResourceSpec1, Kind, 0); C2_OFFSET(FResourceSpec1, Subkind, 4);
    C2_OFFSET(FResourceSpec1, Cell, 8); C2_OFFSET(FResourceSpec1, Location, 12);
    C2_OFFSET(FResourceSpec1, ParentSerial, 16);
    C2_OFFSET(FResourceActual1, NativeValue, 0); C2_OFFSET(FResourceActual1, Extent, 8);
    C2_OFFSET(FResourceActual1, Result, 16); C2_OFFSET(FResourceActual1, SavedError, 20);
    C2_OFFSET(FResourceActual1, Location, 24); C2_OFFSET(FResourceActual1, Reserved, 28);
    C2_OFFSET(FResourceCell1, NativeSerial, 0); C2_OFFSET(FResourceCell1, ParentSerial, 8);
    C2_OFFSET(FResourceCell1, NativeValue, 16); C2_OFFSET(FResourceCell1, Extent, 24);
    C2_OFFSET(FResourceCell1, SourceSerial, 32); C2_OFFSET(FResourceCell1, Kind, 40);
    C2_OFFSET(FResourceCell1, Subkind, 44); C2_OFFSET(FResourceCell1, LocalState, 48);
    C2_OFFSET(FResourceCell1, SavedError, 52); C2_OFFSET(FResourceCell1, SourceLocation, 56);
    C2_OFFSET(FResourceCell1, Result, 60);
    C2_OFFSET(FResourceJournalHeader1, Size, 0); C2_OFFSET(FResourceJournalHeader1, Version, 4);
    C2_OFFSET(FResourceJournalHeader1, Profile, 8); C2_OFFSET(FResourceJournalHeader1, CellCount, 12);
    C2_OFFSET(FResourceJournalHeader1, HookDispatches, 16); C2_OFFSET(FResourceJournalHeader1, NativeIdentityCalls, 24);
    C2_OFFSET(FResourceJournalHeader1, ReconcileCursor, 32); C2_OFFSET(FResourceJournalHeader1, Fault, 36);
    C2_OFFSET(FResourceJournalHeader1, CreateEntered, 40); C2_OFFSET(FResourceJournalHeader1, FirstNativeRefusal, 44);
    C2_OFFSET(FResourceJournalHeader1, ReservedTail, 48); C2_OFFSET(FResourceJournal1, Cells, 64);
    C2_OFFSET(FResourceBinding1, Size, 0); C2_OFFSET(FResourceBinding1, Version, 4);
    C2_OFFSET(FResourceBinding1, Owner, 8); C2_OFFSET(FResourceBinding1, OwnerEpoch, 16);
    C2_OFFSET(FResourceBinding1, ServiceEpoch, 24); C2_OFFSET(FResourceBinding1, Reserve, 32);
    C2_OFFSET(FResourceBinding1, Obtained, 40); C2_OFFSET(FResourceBinding1, NotObtained, 48);
    C2_OFFSET(FResourceBinding1, Closed, 56); C2_OFFSET(FResourceBinding1, Uncertain, 64);
    C2_OFFSET(FResourceBinding1, Profile, 72); C2_OFFSET(FResourceBinding1, CellCount, 76);
    C2_OFFSET(FResourceBinding1, Journal, 80); C2_OFFSET(FResourceBinding1, Reserved, 88);
    C2_OFFSET(FHookOutcome1, Disposition, 0); C2_OFFSET(FHookOutcome1, NativeStatus, 4);
    C2_OFFSET(FHookOutcome1, NativeCalls, 8); C2_OFFSET(FHookOutcome1, Reserved, 12);
    C2_OFFSET(FResourceProgress1, Size, 0); C2_OFFSET(FResourceProgress1, Version, 4);
    C2_OFFSET(FResourceProgress1, VisitedRows, 8); C2_OFFSET(FResourceProgress1, Dispatches, 12);
    C2_OFFSET(FResourceProgress1, NativeCalls, 16); C2_OFFSET(FResourceProgress1, PendingRows, 20);
    C2_OFFSET(FResourceProgress1, OutstandingRows, 24); C2_OFFSET(FResourceProgress1, Fault, 28);
    C2_OFFSET(VrResourceReply1, Size, 0); C2_OFFSET(VrResourceReply1, Version, 4);
    C2_OFFSET(VrResourceReply1, NativeCalls, 8); C2_OFFSET(VrResourceReply1, Reserved, 12);
    C2_OFFSET(VrResourceReply1, Ticket, 16); C2_OFFSET(VrResourceReply1, ReservedTail, 56);
#define C2_MATCH(T, F, U) static_assert(offsetof(T, F) == offsetof(U, F)); \
    static_assert(sizeof(((T*)nullptr)->F) == sizeof(((U*)nullptr)->F))
    C2_MATCH(FResourceTicket1, Size, VrResourceTicket1); C2_MATCH(FResourceTicket1, Version, VrResourceTicket1);
    C2_MATCH(FResourceTicket1, OwnerEpoch, VrResourceTicket1); C2_MATCH(FResourceTicket1, ServiceEpoch, VrResourceTicket1);
    C2_MATCH(FResourceTicket1, Serial, VrResourceTicket1); C2_MATCH(FResourceTicket1, Cell, VrResourceTicket1);
    C2_MATCH(FResourceTicket1, Kind, VrResourceTicket1); C2_MATCH(FResourceTicket1, Subkind, VrResourceTicket1);
    C2_MATCH(FResourceSpec1, Kind, VrResourceSpec1); C2_MATCH(FResourceSpec1, Subkind, VrResourceSpec1);
    C2_MATCH(FResourceSpec1, Cell, VrResourceSpec1); C2_MATCH(FResourceSpec1, Location, VrResourceSpec1);
    C2_MATCH(FResourceSpec1, ParentSerial, VrResourceSpec1);
    C2_MATCH(FResourceActual1, NativeValue, VrResourceActual1); C2_MATCH(FResourceActual1, Extent, VrResourceActual1);
    C2_MATCH(FResourceActual1, Result, VrResourceActual1); C2_MATCH(FResourceActual1, SavedError, VrResourceActual1);
    C2_MATCH(FResourceActual1, Location, VrResourceActual1); C2_MATCH(FResourceActual1, Reserved, VrResourceActual1);
#undef C2_MATCH
#undef C2_OFFSET
    static_assert(std::is_same_v<decltype(FResourceActual1::Result), decltype(VrResourceActual1::Result)>);
    static_assert(std::is_same_v<decltype(FResourceActual1::SavedError), decltype(VrResourceActual1::SavedError)>);
    static_assert(std::is_same_v<VrStatus, int32_t> && sizeof(VrSession) == 16);
    static_assert(static_cast<int32_t>(UINT32_MAX) == -1);
    static_assert(sizeof(FResourceEntryAddress1) == 8 && alignof(FResourceEntryAddress1) == 8);
    static_assert(sizeof(VrPrivateResourceReserve1Fn) == sizeof(FResourceEntryAddress1));
    static_assert(sizeof(VrPrivateResourceObtained1Fn) == sizeof(FResourceEntryAddress1));
    static_assert(sizeof(VrPrivateResourceNotObtained1Fn) == sizeof(FResourceEntryAddress1));
    static_assert(sizeof(VrPrivateResourceClosed1Fn) == sizeof(FResourceEntryAddress1));
    static_assert(sizeof(VrPrivateResourceUncertain1Fn) == sizeof(FResourceEntryAddress1));

    enum EDomain2 : uint32_t { Invalid2, Allocator2, Fd2, Pipe2, Reference2, Bio2, Store2, Index2, Init2 };
    static EDomain2 Domain(uint32_t K, uint32_t S)
    {
        if ((K == 4 && S >= 2 && S <= 15) || (K == 5 && (S == 1 || S == 2))
            || ((K == 6 || K == 7) && S == 1)) return Allocator2;
        if ((K == 5 && S == 3) || (K == 8 && S == 1)) return Fd2;
        if (K == 9 && (S == 1 || S == 2)) return Pipe2;
        if (K != 8) return Invalid2;
        if (S == 2 || S == 3 || S == 6) return Reference2;
        if (S == 4 || S == 5) return Bio2;
        if (S == 7) return Store2;
        if (S == 9 || S == 10) return Index2;
        if (S == 8 || S == 11 || S == 12) return Init2;
        return Invalid2;
    }
    static uint64_t Ceiling(uint32_t K, uint32_t S)
    {
        if (K == 7) return 65536;
        if (K == 5 || K == 6) return 16384;
        switch (S)
        {
        case 2: return 32768; case 3: return 24; case 4: return 512; case 5: return 8;
        case 6: return 4096; case 7: return 65556; case 8: return 256; case 9: return 4096;
        case 10: return 4096; case 11: return 65552; case 12: return 32800;
        case 13: return 8192; case 14: return 32768; case 15: return 65536;
        default: return 0;
        }
    }
    static uint64_t Signed(int32_t R) { return static_cast<uint64_t>(static_cast<int64_t>(R)); }
    static uint64_t InitKey(uint32_t S) { return S == 8 ? 1u : (S == 11 ? 2u : 3u); }
    static bool ReferenceDomain(EDomain2 D) { return D == Reference2 || D == Bio2 || D == Store2; }
    static bool ParentLabel(uint32_t K, uint32_t S, const FResourceCell1& P)
    {
        if (K == 7) return false; // Context is the sole source-pool root.
        if (K == 6) return P.Kind == 7 && P.Subkind == 1;
        if (K == 9) return P.Kind == 5 && P.Subkind == 2;
        if (K == 5) return S == 3 ? (P.Kind == 5 && P.Subkind == 1)
            : (S == 1 ? P.Kind == 6 && P.Subkind == 1 : P.Kind == 7 && P.Subkind == 1);
        if (K == 4)
        {
            if (S == 2 || S == 3 || S == 13 || S == 14) return P.Kind == 7 && P.Subkind == 1;
            if (S == 4 || S == 5) return P.Kind == 6 && P.Subkind == 1;
            return P.Kind == 5 && P.Subkind == 1;
        }
        if (K != 8) return false;
        if (S == 1 || S == 8 || S >= 9) return P.Kind == 7 && P.Subkind == 1;
        if (S == 2) return P.Kind == 6 && P.Subkind == 1;
        if (S == 3) return P.Kind == 5 && P.Subkind == 1;
        if (S == 4) return P.Kind == 8 && P.Subkind == 3;
        if (S == 5 || S == 7) return P.Kind == 8 && P.Subkind == 2;
        return S == 6 && P.Kind == 8 && (P.Subkind == 2 || P.Subkind == 3);
    }
    static uint32_t GlobalCap(uint32_t K, uint32_t S)
    {
        if (K == 7 || K == 9) return 1;
        if (K == 6) return 2;
        if (K == 5) return S == 2 ? 1u : 2u;
        if (K == 4)
        {
            if (S == 2 || S == 3) return 1;
            if (S == 9) return 16;
            return S == 12 ? 4u : 2u;
        }
        if (K == 8)
        {
            if (S == 7) return 32;
            if (S == 6) return 3;
            return S == 2 || S == 3 || S == 4 ? 2u : 1u;
        }
        return 0;
    }
    static uint32_t ParentCap(uint32_t K, uint32_t S)
    {
        if (K == 4 && S == 9) return 8;
        if (K == 4 && (S == 12 || S == 13 || S == 14)) return 2;
        if (K == 8 && S == 7) return 16;
        // Both data WSIs may use the same selected actual vhost.
        return K == 6 || (K == 5 && S == 1) ? 2u : 1u;
    }
    static bool PoolCapacity(const FResourceBinding1& R, const FResourceSpec1& S)
    {
        uint32_t Global = 0, PerParent = 0; uint64_t Bytes = 0;
        for (uint32_t I = 12; I < 140; ++I)
        {
            const FResourceCell1& C = R.Journal->Cells[I];
            const bool Active = C.LocalState != ResourceEmpty1 && C.LocalState != ResourceClosedAck1 && C.LocalState != ResourceNotObtainedAck1;
            if (!Active) continue;
            if (C.Kind == S.Kind && C.Subkind == S.Subkind)
            { ++Global; if (C.ParentSerial == S.ParentSerial) ++PerParent; }
            if (Domain(C.Kind, C.Subkind) != Allocator2) continue;
            const uint64_t Q = C.LocalState == ResourceReserved1 || C.LocalState == ResourceNotObtainedPending1
                || C.LocalState == ResourceAcquireFaultPending2 || C.LocalState == ResourceAcquireFaultAck2
                ? Ceiling(C.Kind, C.Subkind) : C.Extent;
            if (!Q || Q > 1048576 - Bytes) return false;
            Bytes += Q;
        }
        const uint64_t Extra = Domain(S.Kind, S.Subkind) == Allocator2 ? Ceiling(S.Kind, S.Subkind) : 0;
        return Global < GlobalCap(S.Kind, S.Subkind) && PerParent < ParentCap(S.Kind, S.Subkind)
            && Extra <= 1048576 - Bytes;
    }
    static bool PoolIdentity(const FResourceCell1& C)
    {
        if (!C.SourceSerial) return false;
        const EDomain2 D = Domain(C.Kind, C.Subkind);
        if (D == Allocator2) return C.NativeValue && C.Extent && C.Extent <= Ceiling(C.Kind, C.Subkind);
        if (D == Fd2 || D == Pipe2 || D == Index2) return C.NativeValue <= INT32_MAX && C.Extent == 0;
        if (ReferenceDomain(D)) return C.NativeValue && C.Extent <= 1;
        return D == Init2 && C.NativeValue == InitKey(C.Subkind) && C.Extent == 0;
    }
    static int32_t AcquisitionResult(const FResourceCell1& C)
    {
        const EDomain2 D = Domain(C.Kind, C.Subkind);
        if (D == Fd2 || D == Index2) return static_cast<int32_t>(C.NativeValue);
        return D == Pipe2 ? 0 : 1;
    }
    static bool NoAcquire(const FResourceCell1& C)
    {
        if (!C.NativeValue && !C.Extent && !C.Result && !C.SavedError) return true; // Real no-attempt cutoff.
        const EDomain2 D = Domain(C.Kind, C.Subkind);
        if (D == Allocator2) return !C.NativeValue && C.Extent && C.Extent <= Ceiling(C.Kind, C.Subkind) && C.Result == -1;
        if (D == Fd2 || D == Pipe2) return C.NativeValue == UINT64_MAX && !C.Extent && C.Result == -1;
        if (D == Reference2 || D == Bio2) return !C.NativeValue && !C.Extent && C.Result == -1 && !C.SavedError;
        if (D == Store2) return C.NativeValue && !C.Extent && !C.Result && !C.SavedError;
        return D == Index2 && C.Result < 0 && C.NativeValue == Signed(C.Result) && !C.Extent && !C.SavedError;
    }
    static bool AcquireFault(const FResourceCell1& C)
    {
        const EDomain2 D = Domain(C.Kind, C.Subkind);
        if (D == Fd2) return C.Result < -1 && C.NativeValue == Signed(C.Result) && !C.Extent;
        if (D == Pipe2) return C.NativeValue == Signed(static_cast<int32_t>(C.NativeValue)) && !C.Extent;
        if (D == Store2) return C.NativeValue && !C.Extent && C.Result != 0 && C.Result != 1 && !C.SavedError;
        return D == Init2 && C.NativeValue == InitKey(C.Subkind) && !C.Extent && C.Result != 1 && !C.SavedError;
    }
    static bool Disposition(const FResourceCell1& C, bool Uncertain)
    {
        if (!PoolIdentity(C)) return false;
        const EDomain2 D = Domain(C.Kind, C.Subkind);
        if (D == Fd2 || D == Pipe2) return Uncertain ? C.Result != 0 : C.Result == 0 && !C.SavedError;
        if (C.SavedError) return false;
        if (D == Allocator2 || D == Reference2) return (D == Allocator2 || C.Extent == 0) && C.Result == 1;
        if (D == Store2) return C.Extent == 1 && C.Result == 2;
        if (D != Bio2) return false; // No invented init/index inverse.
        return C.Extent == 1 ? C.Subkind == 4 && C.Result == 2
            : (Uncertain ? C.Result != 1 : C.Result == 1);
    }
    static uint32_t Kind(uint32_t I) { return I == 0 ? 4u : (I == 1 ? 1u : (I < 8 ? 2u : 3u)); }
    static uint32_t Subkind(uint32_t I) { return I >= 10 ? 2u : 1u; }
    static bool Terminal(uint32_t S) { return S == ResourceClosedAck1 || S == ResourceNotObtainedAck1; }
    static bool Pending(uint32_t S)
    { return S == ResourceReserved1 || S == ResourceObtainedPending1 || (S >= ResourceNotObtainedPending1 && S <= ResourceObtainedThenUncertain1) || S == ResourceAcquireFaultPending2; }
    static FHookOutcome1 Local(uint32_t D) { return {D, 0, 0, 0}; }
    static bool Aligned(const void* P) { return P && reinterpret_cast<uintptr_t>(P) % 8 == 0; }
    static bool Shape(const FResourceBinding1* R)
    {
        if (!Aligned(R) || R->Size != sizeof(*R) || R->Version != 1 || !Aligned(R->Owner)
            || !R->OwnerEpoch || !R->ServiceEpoch || !R->Reserve || !R->Obtained || !R->NotObtained
            || !R->Closed || !R->Uncertain || R->Profile != 2 || R->CellCount != 140 || R->Reserved
            || !Aligned(R->Journal)) return false;
        const uintptr_t J = reinterpret_cast<uintptr_t>(R->Journal);
        if (J > UINTPTR_MAX - 9248 || reinterpret_cast<uintptr_t>(R) != J + 9072) return false;
        const FResourceJournalHeader1& H = R->Journal->Header;
        if (H.Size != sizeof(H) || H.Version != 1 || H.Profile != 2 || H.CellCount != 140
            || H.ReconcileCursor >= 140 || H.Fault > 7 || H.CreateEntered > 1
            || H.ReservedTail[0] || H.ReservedTail[1]) return false;
        for (uint32_t I = 0; I < 140; ++I)
        {
            const FResourceCell1& C = R->Journal->Cells[I];
            if (C.LocalState > ResourceAcquireFaultAck2 || (I < 12 && C.LocalState > ResourceNotObtainedAck1)) return false;
            if (C.LocalState == ResourceEmpty1)
            {
                if (C.NativeSerial || C.ParentSerial || C.NativeValue || C.Extent || C.SourceSerial
                    || C.Kind || C.Subkind || C.SavedError || C.SourceLocation || C.Result) return false;
                continue;
            }
            if (!C.NativeSerial) return false;
            if (I < 12)
            {
                if (C.Kind != Kind(I) || C.Subkind != Subkind(I)
                    || (I < 10 ? C.ParentSerial != 0 : C.ParentSerial == 0)) return false;
            }
            else
            {
                if (Domain(C.Kind, C.Subkind) == Invalid2 || (C.LocalState != ResourceReserved1 && !C.SourceSerial)) return false;
                if (C.Kind == 7 ? C.ParentSerial != 0 : C.ParentSerial == 0) return false;
                if (C.ParentSerial)
                {
                    const FResourceCell1* Parent = nullptr;
                    for (const FResourceCell1& P : R->Journal->Cells)
                        if (P.LocalState != ResourceEmpty1 && P.NativeSerial == C.ParentSerial) Parent = &P;
                    if (!Parent || C.ParentSerial >= C.NativeSerial || !ParentLabel(C.Kind, C.Subkind, *Parent)) return false;
                }
            }
            for (uint32_t K = 0; K < I; ++K)
            {
                const FResourceCell1& P = R->Journal->Cells[K];
                if (P.LocalState != ResourceEmpty1 && (P.NativeSerial == C.NativeSerial
                    || (I >= 12 && K >= 12 && C.SourceSerial && P.SourceSerial == C.SourceSerial))) return false;
            }
        }
        return true;
    }
    static void Fault(const FResourceBinding1& R, uint32_t Bits, int32_t Status = 0)
    {
        R.Journal->Header.Fault |= Bits;
        if (Status != 0 && R.Journal->Header.FirstNativeRefusal == 0) R.Journal->Header.FirstNativeRefusal = Status;
    }
    static bool CanDispatch(const FResourceBinding1& R)
    {
        FResourceJournalHeader1& H = R.Journal->Header;
        if (H.Fault & (ResourceProtocolFault1 | ResourceRefusalFault1)) return false;
        if (H.HookDispatches == UINT64_MAX || H.NativeIdentityCalls > UINT64_MAX - 2)
        { Fault(R, ResourceProtocolFault1); return false; }
        return true;
    }
    static bool ZeroTicket(const VrResourceTicket1& T)
    { return T.Size == 0 && T.Version == 0 && T.OwnerEpoch == 0 && T.ServiceEpoch == 0 && T.Serial == 0 && T.Cell == 0 && T.Kind == 0 && T.Subkind == 0; }
    static bool Ticket(const FResourceBinding1& R, uint32_t I, const VrResourceTicket1& T, uint64_t Serial)
    {
        return T.Size == sizeof(T) && T.Version == 1 && T.OwnerEpoch == R.OwnerEpoch
            && T.ServiceEpoch == R.ServiceEpoch && T.Serial != 0 && (Serial == 0 || T.Serial == Serial)
            && T.Cell == I && (Serial == 0 || (T.Kind == R.Journal->Cells[I].Kind && T.Subkind == R.Journal->Cells[I].Subkind))
            && (I < 12 ? (T.Kind == Kind(I) && T.Subkind == Subkind(I))
                : Domain(T.Kind, T.Subkind) != Invalid2);
    }
    static FHookOutcome1 Finish(const FResourceBinding1& R, uint32_t I, VrStatus Status,
        const VrResourceReply1& Out, uint64_t Serial)
    {
        if (Status < VR_STATUS_OK || Status > VR_STATUS_RELEASE_UNCERTAIN
            || Out.Size != sizeof(Out) || Out.Version != 1 || Out.Reserved || Out.ReservedTail || Out.NativeCalls > 2)
        { Fault(R, ResourceProtocolFault1, Status); return {ResourceProtocolFailure1, Status, 0, 0}; }
        R.Journal->Header.NativeIdentityCalls += Out.NativeCalls;
        if ((Status == VR_STATUS_OK ? !Ticket(R, I, Out.Ticket, Serial) : !ZeroTicket(Out.Ticket)))
        { Fault(R, ResourceProtocolFault1, Status); return {ResourceProtocolFailure1, Status, 0, 0}; }
        if (Status == VR_STATUS_OK) return {ResourceCompleted1, Status, Out.NativeCalls, 0};
        if (Status == VR_STATUS_BUSY) return {ResourceNativeBusy1, Status, Out.NativeCalls, 0};
        if (R.Journal->Header.FirstNativeRefusal == 0) R.Journal->Header.FirstNativeRefusal = Status;
        return {ResourceNativeRefused1, Status, Out.NativeCalls, 0};
    }
    static bool ValueValid(uint32_t I, const FResourceCell1& C)
    {
        if (I >= 12) return PoolIdentity(C);
        if (I == 0) return C.NativeValue != UINT64_MAX && C.Extent >= 94800 && C.Extent <= 131072
            && C.SourceSerial == 0 && C.SourceLocation == 0;
        if (I == 8 || I == 9) return C.NativeValue != 0 && C.Extent == 0
            && C.SourceSerial == C.NativeValue && C.SourceLocation == I;
        const bool Location = I == 1 ? C.SourceLocation == 1
            : (I < 8 ? C.SourceLocation >= 2 && C.SourceLocation < 8 : C.SourceLocation == I);
        return Location && C.NativeValue <= INT32_MAX && C.Extent == 0 && C.SourceSerial != 0;
    }
    static bool ChildTerminal(const FResourceBinding1& R, uint32_t I)
    {
        if (I < 12)
        {
            if (I != 8 && I != 9) return true;
            const FResourceCell1& Child = R.Journal->Cells[I + 2];
            return Child.LocalState == ResourceEmpty1 || Terminal(Child.LocalState);
        }
        const uint64_t Serial = R.Journal->Cells[I].NativeSerial;
        for (const FResourceCell1& C : R.Journal->Cells)
            if (C.LocalState != ResourceEmpty1 && C.ParentSerial == Serial && !Terminal(C.LocalState)) return false;
        return true;
    }
    static bool BeginRecording(const FResourceBinding1* R, uint32_t I)
    { return Shape(R) && I < 12; }
}

C2_BODY bool ResourceBindingValid1(const FResourceBinding1* R) { return C2ResourceDetail::Shape(R); }
C2_BODY uint32_t ResourceClientCloseCredits1(const FResourceBinding1* R)
{
    if (!C2ResourceDetail::Shape(R)) return 2; // Conservative, never an invalid-shape zero certificate.
    uint32_t N = 0;
    for (uint32_t I = 12; I < 140; ++I)
    {
        const FResourceCell1& C = R->Journal->Cells[I];
        if (C.Kind == 5 && C.Subkind == 3 && (C.LocalState == ResourceObtainedPending1 || C.LocalState == ResourceOwnedAck1)) ++N;
    }
    return N;
}
C2_BODY FHookOutcome1 ResourceReserve1(const FResourceBinding1* R, const FResourceSpec1* S)
{
    if (!C2ResourceDetail::Shape(R) || !C2ResourceDetail::Aligned(S) || S->Cell >= 140 || S->Location != S->Cell)
        return C2ResourceDetail::Local(ResourceLocalInvalid1);
    const bool Prefix = S->Cell < 12;
    if (Prefix ? (S->Kind != C2ResourceDetail::Kind(S->Cell) || S->Subkind != C2ResourceDetail::Subkind(S->Cell)
            || (S->Cell < 10 ? S->ParentSerial != 0 : S->ParentSerial == 0))
        : (C2ResourceDetail::Domain(S->Kind, S->Subkind) == C2ResourceDetail::Invalid2
            || (S->Kind == 7 ? S->ParentSerial != 0 : S->ParentSerial == 0)))
        return C2ResourceDetail::Local(ResourceLocalInvalid1);
    if (!C2ResourceDetail::CanDispatch(*R)) return C2ResourceDetail::Local(ResourceProtocolFailure1);
    FResourceCell1& C = R->Journal->Cells[S->Cell];
    if (R->Journal->Header.Fault != 0 || (C.LocalState != ResourceEmpty1 && (!Prefix || !C2ResourceDetail::Terminal(C.LocalState))))
        return C2ResourceDetail::Local(ResourceLocalInvalid1);
    if (Prefix && S->Cell >= 10)
    {
        const FResourceCell1& P = R->Journal->Cells[S->Cell - 2];
        if (P.NativeSerial != S->ParentSerial || (P.LocalState != ResourceObtainedPending1 && P.LocalState != ResourceOwnedAck1))
            return C2ResourceDetail::Local(ResourceParentDeferred1);
    }
    if (!Prefix && S->ParentSerial)
    {
        const FResourceCell1* Parent = nullptr;
        for (const FResourceCell1& P : R->Journal->Cells)
            if (P.LocalState != ResourceEmpty1 && P.NativeSerial == S->ParentSerial) Parent = &P;
        if (!Parent || !C2ResourceDetail::ParentLabel(S->Kind, S->Subkind, *Parent)
            || (Parent->LocalState != ResourceObtainedPending1 && Parent->LocalState != ResourceOwnedAck1))
            return C2ResourceDetail::Local(ResourceParentDeferred1);
    }
    if (!Prefix && !C2ResourceDetail::PoolCapacity(*R, *S)) return C2ResourceDetail::Local(ResourceQuotaDeferred1);
    if (!C2ResourceDetail::ChildTerminal(*R, S->Cell)) return C2ResourceDetail::Local(ResourceParentDeferred1);
    VrResourceSpec1 Spec{S->Kind, S->Subkind, S->Cell, S->Location, S->ParentSerial};
    VrSession Session{R->OwnerEpoch, R->ServiceEpoch}; VrResourceReply1 Reply{};
    // This exact type restores the original function-pointer type. No invocation
    // of void(*)(), no mirrored-record pointer cast, no directly linked producer.
    const VrPrivateResourceReserve1Fn Fn = reinterpret_cast<VrPrivateResourceReserve1Fn>(R->Reserve);
    ++R->Journal->Header.HookDispatches;
    const VrStatus Status = Fn(static_cast<VrOwner*>(R->Owner), Session, &Spec, sizeof(Reply), &Reply);
    const FHookOutcome1 O = C2ResourceDetail::Finish(*R, S->Cell, Status, Reply, 0);
    if (O.Disposition != ResourceCompleted1) return O;
    if (Reply.Ticket.Kind != S->Kind || Reply.Ticket.Subkind != S->Subkind)
    { C2ResourceDetail::Fault(*R, ResourceProtocolFault1); return {ResourceProtocolFailure1, Status, O.NativeCalls, 0}; }
    for (const FResourceCell1& Old : R->Journal->Cells)
        if (Old.NativeSerial >= Reply.Ticket.Serial)
        { C2ResourceDetail::Fault(*R, ResourceProtocolFault1); return {ResourceProtocolFailure1, Status, 0, 0}; }
    // Durably issue exactly once before C1 can acquire or take a fallible step.
    C.NativeSerial = Reply.Ticket.Serial; C.ParentSerial = S->ParentSerial;
    C.NativeValue = 0; C.Extent = 0; C.SourceSerial = 0;
    C.Kind = S->Kind; C.Subkind = S->Subkind; C.SavedError = 0;
    C.SourceLocation = S->Cell; C.Result = 0; C.LocalState = ResourceReserved1;
    return O;
}

C2_BODY FHookOutcome1 ResourceAcknowledgeNext1(const FResourceBinding1* R, uint32_t I)
{
    if (!C2ResourceDetail::Shape(R) || I >= 140) return C2ResourceDetail::Local(ResourceLocalInvalid1);
    if (!C2ResourceDetail::CanDispatch(*R)) return C2ResourceDetail::Local(ResourceProtocolFailure1);
    FResourceCell1& C = R->Journal->Cells[I]; const uint32_t State = C.LocalState;
    const bool Acquire = State == ResourceObtainedPending1 || State == ResourceObtainedThenClosed1 || State == ResourceObtainedThenUncertain1;
    const bool Not = State == ResourceNotObtainedPending1, Close = State == ResourceClosedPending1;
    const bool AcquireUnknown = State == ResourceAcquireFaultPending2;
    const bool Uncertain = State == ResourceUncertainPending1 || AcquireUnknown;
    if (!Acquire && !Not && !Close && !Uncertain) return C2ResourceDetail::Local(ResourceParentDeferred1);
    if ((Close || Not || Uncertain) && !C2ResourceDetail::ChildTerminal(*R, I)) return C2ResourceDetail::Local(ResourceParentDeferred1);
    if (!Not && !AcquireUnknown && !C2ResourceDetail::ValueValid(I, C)) { C2ResourceDetail::Fault(*R, ResourceProtocolFault1); return C2ResourceDetail::Local(ResourceProtocolFailure1); }
    VrResourceTicket1 T{40, 1, R->OwnerEpoch, R->ServiceEpoch, C.NativeSerial, I,
        static_cast<uint16_t>(C.Kind), static_cast<uint16_t>(C.Subkind)};
    VrResourceActual1 A{}; A.Location = I;
    if (I >= 12)
    {
        if ((Not && !C2ResourceDetail::NoAcquire(C)) || (AcquireUnknown && !C2ResourceDetail::AcquireFault(C))
            || (Close && !C2ResourceDetail::Disposition(C, false))
            || (Uncertain && !AcquireUnknown && !C2ResourceDetail::Disposition(C, true)))
        { C2ResourceDetail::Fault(*R, ResourceProtocolFault1); return C2ResourceDetail::Local(ResourceProtocolFailure1); }
        A.NativeValue = C.NativeValue;
        A.Extent = Acquire && C2ResourceDetail::ReferenceDomain(C2ResourceDetail::Domain(C.Kind, C.Subkind)) ? 0 : C.Extent;
        A.Result = Acquire ? C2ResourceDetail::AcquisitionResult(C) : C.Result;
        A.SavedError = Acquire ? 0 : static_cast<int32_t>(C.SavedError);
    }
    else
    {
    if (Not)
    {
        if (C.NativeValue || C.Extent || (C.Result != 0 && C.Result != -1)
            || (C.Result == 0 && C.SavedError != 0) || ((I == 8 || I == 9) && C.Result != 0))
        { C2ResourceDetail::Fault(*R, ResourceProtocolFault1); return C2ResourceDetail::Local(ResourceProtocolFailure1); }
        A.Result = C.Result; A.SavedError = static_cast<int32_t>(C.SavedError);
    }
    else
    {
        A.NativeValue = C.NativeValue; A.Extent = C.Extent;
        A.Result = Acquire ? ((I == 0 || I == 8 || I == 9) ? 0 : static_cast<int32_t>(C.NativeValue)) : C.Result;
        A.SavedError = Acquire ? 0 : static_cast<int32_t>(C.SavedError);
        if ((Close && (A.Result != 0 || A.SavedError != 0)) || (Uncertain && A.Result == 0))
        { C2ResourceDetail::Fault(*R, ResourceProtocolFault1); return C2ResourceDetail::Local(ResourceProtocolFailure1); }
    }
    }
    VrSession Session{R->OwnerEpoch, R->ServiceEpoch}; VrResourceReply1 Reply{}; VrStatus Status;
    ++R->Journal->Header.HookDispatches;
    if (Acquire)
    {
        const VrPrivateResourceObtained1Fn Fn = reinterpret_cast<VrPrivateResourceObtained1Fn>(R->Obtained);
        Status = Fn(static_cast<VrOwner*>(R->Owner), Session, &T, &A, sizeof(Reply), &Reply);
    }
    else if (Not)
    {
        const VrPrivateResourceNotObtained1Fn Fn = reinterpret_cast<VrPrivateResourceNotObtained1Fn>(R->NotObtained);
        Status = Fn(static_cast<VrOwner*>(R->Owner), Session, &T, &A, sizeof(Reply), &Reply);
    }
    else if (Close)
    {
        const VrPrivateResourceClosed1Fn Fn = reinterpret_cast<VrPrivateResourceClosed1Fn>(R->Closed);
        Status = Fn(static_cast<VrOwner*>(R->Owner), Session, &T, &A, sizeof(Reply), &Reply);
    }
    else
    {
        const VrPrivateResourceUncertain1Fn Fn = reinterpret_cast<VrPrivateResourceUncertain1Fn>(R->Uncertain);
        Status = Fn(static_cast<VrOwner*>(R->Owner), Session, &T, &A, sizeof(Reply), &Reply);
    }
    const FHookOutcome1 O = C2ResourceDetail::Finish(*R, I, Status, Reply, C.NativeSerial);
    if (O.Disposition == ResourceNativeRefused1) C2ResourceDetail::Fault(*R, ResourceRefusalFault1, Status);
    if (O.Disposition != ResourceCompleted1) return O;
    if (State == ResourceObtainedThenClosed1) C.LocalState = ResourceClosedPending1;
    else if (State == ResourceObtainedThenUncertain1) C.LocalState = ResourceUncertainPending1;
    else if (Acquire) C.LocalState = ResourceOwnedAck1;
    else if (Not) C.LocalState = ResourceNotObtainedAck1;
    else if (Close) C.LocalState = ResourceClosedAck1;
    else C.LocalState = AcquireUnknown ? ResourceAcquireFaultAck2 : ResourceUncertaintyAck1;
    // In particular terminal Result/Error are never overwritten on prior-Obtained.
    return O;
}

C2_BODY void ResourceRecordFdResult1(const FResourceBinding1* R, uint32_t I,
    uint64_t Serial, uint32_t Location, int32_t Fd, int32_t Error)
{
    if (!C2ResourceDetail::BeginRecording(R, I)) return; // Actual caller has already proved this before OS.
    FResourceCell1& C = R->Journal->Cells[I];
    if (I == 0 || I == 8 || I == 9 || C.LocalState != ResourceReserved1 || !Serial
        || C.SourceSerial != Serial || C.SourceLocation != Location)
    { C2ResourceDetail::Fault(*R, ResourceProtocolFault1 | (Fd >= 0 ? ResourceUncertainFault1 : 0)); return; }
    C.SourceSerial = Serial; C.SourceLocation = Location;
    C.NativeValue = Fd >= 0 ? static_cast<uint64_t>(Fd) : 0; C.Extent = 0;
    C.Result = Fd >= 0 ? Fd : -1; C.SavedError = Fd >= 0 ? 0 : static_cast<uint32_t>(Error);
    C.LocalState = Fd >= 0 ? ResourceObtainedPending1 : ResourceNotObtainedPending1;
}
C2_BODY void ResourceRecordMapResult1(const FResourceBinding1* R, void* Raw, uint64_t Bytes, int32_t Error)
{
    if (!C2ResourceDetail::BeginRecording(R, 0)) return;
    FResourceCell1& C = R->Journal->Cells[0];
    if (C.LocalState != ResourceReserved1 || Bytes < 94800 || Bytes > 131072)
    { C2ResourceDetail::Fault(*R, ResourceProtocolFault1 | (reinterpret_cast<uintptr_t>(Raw) != UINTPTR_MAX ? ResourceUncertainFault1 : 0)); return; }
    const bool Obtained = reinterpret_cast<uintptr_t>(Raw) != UINTPTR_MAX;
    C.NativeValue = Obtained ? reinterpret_cast<uintptr_t>(Raw) : 0; C.Extent = Obtained ? Bytes : 0;
    C.SourceSerial = 0; C.SourceLocation = 0; C.Result = Obtained ? 0 : -1;
    C.SavedError = Obtained ? 0 : static_cast<uint32_t>(Error);
    C.LocalState = Obtained ? ResourceObtainedPending1 : ResourceNotObtainedPending1;
}
C2_BODY void ResourceRecordNotAttempted1(const FResourceBinding1* R, uint32_t I)
{
    if (!C2ResourceDetail::BeginRecording(R, I)) return;
    FResourceCell1& C = R->Journal->Cells[I];
    if (C.LocalState != ResourceReserved1) { C2ResourceDetail::Fault(*R, ResourceProtocolFault1); return; }
    C.NativeValue = 0; C.Extent = 0; C.Result = 0; C.SavedError = 0; C.LocalState = ResourceNotObtainedPending1;
}
C2_BODY void ResourceRecordFdDisposition1(const FResourceBinding1* R, uint32_t I,
    uint64_t Serial, int32_t OriginalFd, int32_t Result, int32_t Error)
{
    if (!C2ResourceDetail::BeginRecording(R, I)) return;
    FResourceCell1& C = R->Journal->Cells[I];
    if (I == 0 || I == 8 || I == 9 || OriginalFd < 0 || C.SourceSerial != Serial
        || C.NativeValue != static_cast<uint64_t>(OriginalFd) || C.Extent
        || (C.LocalState != ResourceObtainedPending1 && C.LocalState != ResourceOwnedAck1))
    { C2ResourceDetail::Fault(*R, ResourceProtocolFault1 | ResourceUncertainFault1); return; }
    const bool PendingAcquire = C.LocalState == ResourceObtainedPending1;
    C.Result = Result; C.SavedError = Result == 0 ? 0 : static_cast<uint32_t>(Error);
    C.LocalState = Result == 0 ? (PendingAcquire ? ResourceObtainedThenClosed1 : ResourceClosedPending1)
        : (PendingAcquire ? ResourceObtainedThenUncertain1 : ResourceUncertainPending1);
    if (Result != 0) C2ResourceDetail::Fault(*R, ResourceUncertainFault1);
}
C2_BODY void ResourceRecordPairFinalized1(const FResourceBinding1* R, uint32_t Slot, uint64_t Generation)
{
    if (!C2ResourceDetail::BeginRecording(R, 8 + Slot) || Slot >= 2) return;
    FResourceCell1& C = R->Journal->Cells[8 + Slot];
    if (!Generation || C.SourceSerial != Generation || C.NativeValue != Generation
        || (C.LocalState != ResourceObtainedPending1 && C.LocalState != ResourceOwnedAck1))
    { C2ResourceDetail::Fault(*R, ResourceProtocolFault1); return; }
    C.Result = 0; C.SavedError = 0;
    C.LocalState = C.LocalState == ResourceObtainedPending1 ? ResourceObtainedThenClosed1 : ResourceClosedPending1;
}
}

namespace C2LSourceDetail
{
    using namespace VaRest::WebSocket::NumericConnectorC1;
    namespace D = VaRest::WebSocket::NumericConnectorC1::C2ResourceDetail;
    static bool Span(const void* P, uint64_t N, uint32_t A)
    { return P && reinterpret_cast<uintptr_t>(P) % A == 0 && N <= UINTPTR_MAX - reinterpret_cast<uintptr_t>(P); }
    static bool Separate(const void* A, uint64_t N, const void* B, uint64_t M)
    {
        const uintptr_t X = reinterpret_cast<uintptr_t>(A), Y = reinterpret_cast<uintptr_t>(B);
        return N <= UINTPTR_MAX - X && M <= UINTPTR_MAX - Y && (X + N <= Y || Y + M <= X);
    }
    static const FResourceBinding1* View(const VrLwsCustody1* V)
    {
        if (!Span(V, sizeof(*V), 8)) return nullptr;
        const auto* R = static_cast<const FResourceBinding1*>(V->ResourceBinding); // Actual original object roundtrip.
        if (!D::Shape(R)) return nullptr;
        const uintptr_t J = reinterpret_cast<uintptr_t>(R->Journal);
        if (reinterpret_cast<uintptr_t>(V) != J + 9232 || reinterpret_cast<uintptr_t>(V->Control) != J + 9168
            || V->Control->Reserved0 || V->Control->Reserved1) return nullptr;
        return R;
    }
    static bool External(const FResourceBinding1* R, const void* P, uint64_t N, uint32_t A)
    { return Span(P, N, A) && Separate(R->Journal, 9248, P, N); }
    static bool Row(const FResourceBinding1* R, const VrLwsRowRef1* Ref)
    {
        if (!External(R, Ref, sizeof(*Ref), 8) || Ref->Reserved || Ref->Cell < 12 || Ref->Cell >= 140 || !Ref->SourceSerial) return false;
        const FResourceCell1& C = R->Journal->Cells[Ref->Cell];
        return C.NativeSerial && C.LocalState != ResourceEmpty1 && C.SourceSerial == Ref->SourceSerial;
    }
    static void Outcome(VrLwsHookOutcome1& To, const FHookOutcome1& From)
    { To.Disposition = From.Disposition; To.NativeStatus = From.NativeStatus; To.NativeCalls = From.NativeCalls; To.Reserved = 0; }
    static bool Output(const FResourceBinding1* R, VrLwsHookOutcome1* Out)
    {
        if (!External(R, Out, sizeof(*Out), 4)) return false;
        return true; // Writes start ONLY after all input/budget aliases are excluded.
    }
    static bool Phase(const FResourceBinding1* R, const VrLwsPhaseBudget1* P)
    {
        if (!External(R, P, sizeof(*P), 4) || P->Reserved) return false;
        const uint32_t Cap = P->Phase == VR_LWS_PHASE_STARTUP1 ? 1536u : (P->Phase == VR_LWS_PHASE_FINAL1 ? 2048u : 0u);
        return Cap && P->Used <= Cap && P->Remaining == Cap - P->Used;
    }
    static bool Budget(const FResourceBinding1* R, const VrLwsBudgetRef1* B)
    {
        return External(R, B, sizeof(*B), 8) && ((B->Active != nullptr) != (B->Creating != nullptr))
            && (B->Active ? External(R, B->Active, sizeof(*B->Active), 8) : Phase(R, B->Creating));
    }
    static bool BudgetExternal(const FResourceBinding1* R, const VrLwsBudgetRef1* B, const void* P, uint64_t N)
    {
        if (!Budget(R, B) || !External(R, P, N, 1) || !Separate(P, N, B, sizeof(*B))) return false;
        if (B->Active) return Separate(P, N, B->Active, sizeof(*B->Active)) && ResourceBorrowedExternal1(B->Active, R, P, N);
        return Separate(P, N, B->Creating, sizeof(*B->Creating));
    }
    // Conservative remaining source disposal/one-next-event obligations, not provider work or authority.
    static uint32_t RequiredGas(const FResourceBinding1* R)
    {
        uint32_t N = 0;
        for (const FResourceCell1& C : R->Journal->Cells)
        {
            if (C.LocalState == ResourceReserved1 || C.LocalState == ResourceNotObtainedPending1
                || C.LocalState == ResourceClosedPending1 || C.LocalState == ResourceUncertainPending1
                || C.LocalState == ResourceAcquireFaultPending2) N += 2;
            else if (C.LocalState == ResourceObtainedPending1) N += 5;
            else if (C.LocalState == ResourceOwnedAck1) N += 3;
            else if (C.LocalState == ResourceObtainedThenClosed1 || C.LocalState == ResourceObtainedThenUncertain1) N += 4;
        }
        return N;
    }
    static FHookOutcome1 Dispatch(const FResourceBinding1* R, const VrLwsBudgetRef1* B,
        const FResourceSpec1* Spec, uint32_t Cell)
    {
        if (!Budget(R, B)) return D::Local(ResourceLocalInvalid1);
        if (B->Active) return ResourceDispatchBorrowed1(B->Active, R, Spec, Cell);
        VrLwsPhaseBudget1& P = *B->Creating;
        const uint32_t Required = RequiredGas(R);
        // Fresh Reserve must leave its new never-attempted ACK credit. ACK may use its own reserved2.
        const uint32_t Retain = Spec ? Required + 2 : (Required >= 2 ? Required - 2 : 0);
        if (P.Remaining < 2 || P.Remaining - 2 < Retain) return D::Local(ResourceQuotaDeferred1);
        P.Remaining -= 2; P.Used += 2;
        const uint64_t Before = R->Journal->Header.HookDispatches;
        const FHookOutcome1 O = Spec ? ResourceReserve1(R, Spec) : ResourceAcknowledgeNext1(R, Cell);
        const uint32_t Cost = R->Journal->Header.HookDispatches == Before ? 0u : (O.Disposition == ResourceProtocolFailure1 ? 2u : O.NativeCalls);
        // ONLY internal actual producer-cost refund; there is no caller refund setter.
        P.Remaining += 2 - Cost; P.Used -= 2 - Cost; return O;
    }
    static FResourceCell1* Reserved(const VrLwsCustody1* V, const VrLwsRowRef1* Ref)
    {
        const FResourceBinding1* R = View(V);
        if (!R || !Row(R, Ref)) return nullptr;
        FResourceCell1& C = R->Journal->Cells[Ref->Cell];
        if (C.LocalState != ResourceReserved1) { D::Fault(*R, ResourceProtocolFault1); return nullptr; }
        return &C;
    }
    static void Store(FResourceCell1& C, uint64_t V, uint64_t X, int32_t R, int32_t E, uint32_t Location, uint32_t State)
    {
        C.NativeValue = V; C.Extent = X; C.Result = R; C.SavedError = static_cast<uint32_t>(E);
        C.SourceLocation = Location; C.LocalState = State;
    }
    static void Bad(const VrLwsCustody1* V)
    { if (const FResourceBinding1* R = View(V)) D::Fault(*R, ResourceProtocolFault1 | ResourceUncertainFault1); }
    static FResourceCell1* Owned(const VrLwsCustody1* V, const VrLwsRowRef1* Ref, const void* Pointer)
    {
        const FResourceBinding1* R = View(V);
        if (!R || !Row(R, Ref)) return nullptr;
        FResourceCell1& C = R->Journal->Cells[Ref->Cell];
        if ((C.LocalState != ResourceObtainedPending1 && C.LocalState != ResourceOwnedAck1)
            || !D::PoolIdentity(C) || C.NativeValue != reinterpret_cast<uintptr_t>(Pointer))
        { Bad(V); return nullptr; }
        return &C;
    }
    static void Terminal(const VrLwsCustody1* V, FResourceCell1& C, uint64_t X, int32_t R, int32_t E, uint32_t Location, bool Unknown)
    {
        const bool Pending = C.LocalState == ResourceObtainedPending1;
        C.Extent = X; C.Result = R; C.SavedError = static_cast<uint32_t>(E); C.SourceLocation = Location;
        C.LocalState = Unknown ? (Pending ? ResourceObtainedThenUncertain1 : ResourceUncertainPending1)
            : (Pending ? ResourceObtainedThenClosed1 : ResourceClosedPending1);
        if (Unknown) D::Fault(*View(V), ResourceUncertainFault1);
    }
}

extern "C"
{
C2_BODY void varest_ws_c2l_prepare_spec1(const VrLwsCustody1* V, const VrLwsRowRef1* Parent,
    uint32_t Kind, uint32_t Subkind, VrLwsSpec1* Spec, VrLwsHookOutcome1* Out)
{
    using namespace C2LSourceDetail;
    const FResourceBinding1* R = View(V);
    if (!R || !Output(R, Out) || !External(R, Spec, sizeof(*Spec), 8)
        || !C2LSourceDetail::Separate(Spec, sizeof(*Spec), Out, sizeof(*Out))
        || (Parent && (!External(R, Parent, sizeof(*Parent), 8)
            || !C2LSourceDetail::Separate(Parent, sizeof(*Parent), Spec, sizeof(*Spec))
            || !C2LSourceDetail::Separate(Parent, sizeof(*Parent), Out, sizeof(*Out))))) return;
    *Out = {VR_LWS_HOOK_LOCAL_INVALID1, 0, 0, 0}; *Spec = {};
    if (D::Domain(Kind, Subkind) == D::Invalid2) return;
    if (R->Journal->Header.Fault || V->Control->NextSourceSerial == UINT64_MAX)
    { Out->Disposition = ResourceProtocolFailure1; return; }
    uint64_t NativeParent = 0;
    if (Kind == 7 && Subkind == 1)
    { if (Parent) return; }
    else
    {
        if (!Parent || !Row(R, Parent)) { Out->Disposition = ResourceParentDeferred1; return; }
        const FResourceCell1& P = R->Journal->Cells[Parent->Cell];
        if ((P.LocalState != ResourceObtainedPending1 && P.LocalState != ResourceOwnedAck1)
            || !D::PoolIdentity(P) || !D::ParentLabel(Kind, Subkind, P))
        { Out->Disposition = ResourceParentDeferred1; return; }
        NativeParent = P.NativeSerial; // Genuine original C++ row, never SourceSerial==NativeSerial.
    }
    uint32_t Cell = 140;
    for (uint32_t I = 12; I < 140; ++I)
        if (R->Journal->Cells[I].LocalState == ResourceEmpty1) { Cell = I; break; }
    if (Cell == 140) { Out->Disposition = ResourceQuotaDeferred1; return; }
    const FResourceSpec1 Candidate{Kind, Subkind, Cell, Cell, NativeParent};
    if (!D::PoolCapacity(*R, Candidate)) { Out->Disposition = ResourceQuotaDeferred1; return; }
    Spec->Kind = Kind; Spec->Subkind = Subkind; Spec->Cell = Cell; Spec->Location = Cell; Spec->ParentSerial = NativeParent;
    Out->Disposition = ResourceCompleted1; // Pure output only: no row/cursor/serial reservation, native or OS call.
    // Same exclusive source interval immediately calls reserve1 ONCE; it revalidates.
}
C2_BODY void varest_ws_c2l_reserve1(const VrLwsCustody1* V, const VrLwsSpec1* Spec,
    const VrLwsBudgetRef1* Budget, VrLwsRowRef1* Ref, VrLwsHookOutcome1* Out)
{
    using namespace C2LSourceDetail;
    const FResourceBinding1* R = View(V);
    if (!R || !Output(R, Out) || !External(R, Ref, sizeof(*Ref), 8)
        || !External(R, Spec, sizeof(*Spec), 8) || !Budget || !C2LSourceDetail::Separate(Out, sizeof(*Out), Ref, sizeof(*Ref))
        || !C2LSourceDetail::Separate(Out, sizeof(*Out), Spec, sizeof(*Spec)) || !C2LSourceDetail::Separate(Ref, sizeof(*Ref), Spec, sizeof(*Spec))
        || !BudgetExternal(R, Budget, Out, sizeof(*Out)) || !BudgetExternal(R, Budget, Ref, sizeof(*Ref))
        || !BudgetExternal(R, Budget, Spec, sizeof(*Spec))) return;
    *Out = {VR_LWS_HOOK_LOCAL_INVALID1, 0, 0, 0}; *Ref = {0, 0, 0};
    if (Spec->Cell < 12 || Spec->Cell >= 140 || Spec->Location != Spec->Cell || !C2LSourceDetail::Budget(R, Budget)) return;
    // This is the actual monotonic completed-Reserve counter. The startup
    // envelope is not a pool cursor and retirement never resets it.
    if (Budget->Creating && Budget->Creating->Phase == VR_LWS_PHASE_STARTUP1
        && V->Control->NextSourceSerial >= 128)
    { Out->Disposition = ResourceQuotaDeferred1; return; } // No dispatch/primitive/refund.
    if (V->Control->NextSourceSerial == UINT64_MAX)
    { V->Control->Fault |= 1; D::Fault(*R, ResourceProtocolFault1); Out->Disposition = ResourceProtocolFailure1; return; }
    // Original real C++ Spec, no mirrored-record pointer punning.
    const FResourceSpec1 S{Spec->Kind, Spec->Subkind, Spec->Cell, Spec->Location, Spec->ParentSerial};
    const FHookOutcome1 O = Dispatch(R, Budget, &S, S.Cell); Outcome(*Out, O);
    if (O.Disposition != ResourceCompleted1) return;
    FResourceCell1& C = R->Journal->Cells[S.Cell];
    C.SourceSerial = ++V->Control->NextSourceSerial; // Durable before real acquire/fallible work.
    Ref->Cell = S.Cell; Ref->Reserved = 0; Ref->SourceSerial = C.SourceSerial;
}
C2_BODY void varest_ws_c2l_observe_row1(const VrLwsCustody1* V, const VrLwsRowRef1* Ref,
    uint32_t* State, VrLwsHookOutcome1* Out)
{
    using namespace C2LSourceDetail;
    const FResourceBinding1* R = View(V);
    if (!R || !Output(R, Out) || !External(R, Ref, sizeof(*Ref), 8)
        || !External(R, State, sizeof(*State), 4)
        || !C2LSourceDetail::Separate(Ref, sizeof(*Ref), State, sizeof(*State))
        || !C2LSourceDetail::Separate(Ref, sizeof(*Ref), Out, sizeof(*Out))
        || !C2LSourceDetail::Separate(State, sizeof(*State), Out, sizeof(*Out))) return;
    *State = UINT32_MAX; *Out = {VR_LWS_HOOK_LOCAL_INVALID1, 0, 0, 0};
    if (!Row(R, Ref)) return;
    const FResourceCell1& C = R->Journal->Cells[Ref->Cell];
    if (C.LocalState < ResourceReserved1 || C.LocalState > ResourceAcquireFaultAck2) return;
    *State = C.LocalState;
    Out->Disposition = ResourceCompleted1;
    // Actual source observation only; it neither consumes an ACK nor establishes
    // physical retirement, actor authority or a native success/status receipt.
}
C2_BODY void varest_ws_c2l_ack_next1(const VrLwsCustody1* V, uint32_t Cell,
    const VrLwsBudgetRef1* Budget, VrLwsHookOutcome1* Out)
{
    using namespace C2LSourceDetail;
    const FResourceBinding1* R = View(V);
    if (!R || !Output(R, Out) || !BudgetExternal(R, Budget, Out, sizeof(*Out))) return;
    *Out = {VR_LWS_HOOK_LOCAL_INVALID1, 0, 0, 0};
    if (Cell < 12 || Cell >= 140) return;
    Outcome(*Out, Dispatch(R, Budget, nullptr, Cell));
}
C2_BODY void varest_ws_c2l_record_never_attempted1(const VrLwsCustody1* V, const VrLwsRowRef1* Ref, uint32_t Location)
{
    using namespace C2LSourceDetail;
    if (FResourceCell1* C = Reserved(V, Ref)) Store(*C, 0, 0, 0, 0, Location, ResourceNotObtainedPending1);
}
C2_BODY void varest_ws_c2l_record_fd_acquire1(const VrLwsCustody1* V, const VrLwsRowRef1* Ref,
    int32_t Fd, int32_t Error, uint32_t Location)
{
    using namespace C2LSourceDetail;
    FResourceCell1* C = Reserved(V, Ref); if (!C) return;
    if (D::Domain(C->Kind, C->Subkind) != D::Fd2) { Bad(V); return; }
    Store(*C, D::Signed(Fd), 0, Fd, Fd >= 0 ? 0 : Error, Location,
        Fd >= 0 ? ResourceObtainedPending1 : (Fd == -1 ? ResourceNotObtainedPending1 : ResourceAcquireFaultPending2));
    if (Fd < -1) D::Fault(*View(V), ResourceUncertainFault1);
}
C2_BODY void varest_ws_c2l_record_pipe_acquire1(const VrLwsCustody1* V, const VrLwsRowRef1* Read,
    const VrLwsRowRef1* Write, const VrLwsPipePairRaw1* Raw, uint32_t Location)
{
    using namespace C2LSourceDetail;
    FResourceCell1* A = Reserved(V, Read); FResourceCell1* B = Reserved(V, Write);
    if (!A || !B || !External(View(V), Raw, sizeof(*Raw), 4)) return;
    if (!C2LSourceDetail::Separate(Raw, sizeof(*Raw), Read, sizeof(*Read)) || !C2LSourceDetail::Separate(Raw, sizeof(*Raw), Write, sizeof(*Write)))
    { Bad(V); return; }
    if (A == B || A->Kind != 9 || A->Subkind != 1 || B->Kind != 9 || B->Subkind != 2
        || A->ParentSerial != B->ParentSerial) { Bad(V); return; }
    const bool Yes = Raw->Result == 0 && Raw->Fds[0] >= 0 && Raw->Fds[1] >= 0 && Raw->Fds[0] != Raw->Fds[1];
    const bool No = Raw->Result == -1 && Raw->Fds[0] == -1 && Raw->Fds[1] == -1;
    const uint32_t State = Yes ? ResourceObtainedPending1 : (No ? ResourceNotObtainedPending1 : ResourceAcquireFaultPending2);
    Store(*A, D::Signed(Raw->Fds[0]), 0, Raw->Result, Yes ? 0 : Raw->SavedError, Location, State);
    Store(*B, D::Signed(Raw->Fds[1]), 0, Raw->Result, Yes ? 0 : Raw->SavedError, Location, State);
    if (!Yes && !No) D::Fault(*View(V), ResourceUncertainFault1);
}
C2_BODY void varest_ws_c2l_record_pointer_acquire1(const VrLwsCustody1* V, const VrLwsRowRef1* Ref,
    const void* Pointer, uint64_t Bytes, int32_t Error, uint32_t Location)
{
    using namespace C2LSourceDetail;
    FResourceCell1* C = Reserved(V, Ref); if (!C) return;
    const auto Domain = D::Domain(C->Kind, C->Subkind);
    if (Domain == D::Allocator2 ? (!Bytes || Bytes > D::Ceiling(C->Kind, C->Subkind))
        : ((Domain != D::Reference2 && Domain != D::Bio2) || Bytes)) { Bad(V); return; }
    Store(*C, reinterpret_cast<uintptr_t>(Pointer), Bytes, Pointer ? 1 : -1,
        !Pointer && Domain == D::Allocator2 ? Error : 0, Location,
        Pointer ? ResourceObtainedPending1 : ResourceNotObtainedPending1);
}
C2_BODY void varest_ws_c2l_record_store_ref_acquire1(const VrLwsCustody1* V, const VrLwsRowRef1* Ref,
    const void* Argument, int32_t Result, uint32_t Location)
{
    using namespace C2LSourceDetail;
    FResourceCell1* C = Reserved(V, Ref); if (!C) return;
    if (!Argument || D::Domain(C->Kind, C->Subkind) != D::Store2) { Bad(V); return; }
    Store(*C, reinterpret_cast<uintptr_t>(Argument), 0, Result, 0, Location,
        Result == 1 ? ResourceObtainedPending1 : (Result == 0 ? ResourceNotObtainedPending1 : ResourceAcquireFaultPending2));
    if (Result != 0 && Result != 1) D::Fault(*View(V), ResourceUncertainFault1);
}
C2_BODY void varest_ws_c2l_record_index_acquire1(const VrLwsCustody1* V, const VrLwsRowRef1* Ref,
    int32_t Index, uint32_t Location)
{
    using namespace C2LSourceDetail;
    FResourceCell1* C = Reserved(V, Ref); if (!C) return;
    if (D::Domain(C->Kind, C->Subkind) != D::Index2) { Bad(V); return; }
    // Qualified matching provider source must prove negative means no issued index.
    Store(*C, D::Signed(Index), 0, Index, 0, Location, Index >= 0 ? ResourceObtainedPending1 : ResourceNotObtainedPending1);
}
C2_BODY void varest_ws_c2l_record_init_acquire1(const VrLwsCustody1* V, const VrLwsRowRef1* Ref,
    int32_t Result, uint32_t Location)
{
    using namespace C2LSourceDetail;
    FResourceCell1* C = Reserved(V, Ref); if (!C) return;
    if (D::Domain(C->Kind, C->Subkind) != D::Init2) { Bad(V); return; }
    Store(*C, D::InitKey(C->Subkind), 0, Result, 0, Location,
        Result == 1 ? ResourceObtainedPending1 : ResourceAcquireFaultPending2);
    if (Result != 1) D::Fault(*View(V), ResourceUncertainFault1);
}
C2_BODY void varest_ws_c2l_record_fd_dispose1(const VrLwsCustody1* V, const VrLwsRowRef1* Ref,
    int32_t Fd, int32_t Result, int32_t Error, uint32_t Location)
{
    using namespace C2LSourceDetail;
    if (Fd < 0) { Bad(V); return; }
    FResourceCell1* C = C2LSourceDetail::Owned(V, Ref, reinterpret_cast<const void*>(static_cast<uintptr_t>(Fd))); if (!C) return;
    const auto Domain = D::Domain(C->Kind, C->Subkind);
    if (Domain != D::Fd2 && Domain != D::Pipe2) { Bad(V); return; }
    Terminal(V, *C, 0, Result, Result == 0 ? 0 : Error, Location, Result != 0);
}
C2_BODY void varest_ws_c2l_record_void_dispose_return1(const VrLwsCustody1* V, const VrLwsRowRef1* Ref,
    const void* Pointer, uint64_t Bytes, uint32_t Location)
{
    using namespace C2LSourceDetail;
    FResourceCell1* C = C2LSourceDetail::Owned(V, Ref, Pointer); if (!C) return;
    const auto Domain = D::Domain(C->Kind, C->Subkind);
    if ((Domain != D::Allocator2 && Domain != D::Reference2) || C->Extent != Bytes) { Bad(V); return; }
    Terminal(V, *C, Bytes, 1, 0, Location, false);
}
C2_BODY void varest_ws_c2l_record_void_dispose_uncertain_return1(const VrLwsCustody1* V, const VrLwsRowRef1* Ref,
    const void* Pointer, uint64_t Bytes, uint32_t Location)
{
    using namespace C2LSourceDetail;
    FResourceCell1* C = C2LSourceDetail::Owned(V, Ref, Pointer); if (!C) return;
    const auto Domain = D::Domain(C->Kind, C->Subkind);
    if ((Domain != D::Allocator2 && Domain != D::Reference2) || C->Extent != Bytes) { Bad(V); return; }
    Terminal(V, *C, Bytes, 1, 0, Location, true);
}
C2_BODY void varest_ws_c2l_record_bio_dispose_return1(const VrLwsCustody1* V, const VrLwsRowRef1* Ref,
    const void* Pointer, int32_t Result, uint32_t Location)
{
    using namespace C2LSourceDetail;
    FResourceCell1* C = C2LSourceDetail::Owned(V, Ref, Pointer); if (!C) return;
    if (D::Domain(C->Kind, C->Subkind) != D::Bio2) { Bad(V); return; }
    Terminal(V, *C, 0, Result, 0, Location, Result != 1);
}
C2_BODY void varest_ws_c2l_record_parent_release_return1(const VrLwsCustody1* V, const VrLwsRowRef1* Ref,
    const void* Pointer, uint32_t Location)
{
    using namespace C2LSourceDetail;
    FResourceCell1* C = C2LSourceDetail::Owned(V, Ref, Pointer); if (!C) return;
    if (!(C->Kind == 8 && (C->Subkind == 4 || C->Subkind == 7))) { Bad(V); return; }
    Terminal(V, *C, 1, 2, 0, Location, false);
}
C2_BODY void varest_ws_c2l_record_parent_release_uncertain_return1(const VrLwsCustody1* V, const VrLwsRowRef1* Ref,
    const void* Pointer, uint32_t Location)
{
    using namespace C2LSourceDetail;
    FResourceCell1* C = C2LSourceDetail::Owned(V, Ref, Pointer); if (!C) return;
    if (!(C->Kind == 8 && (C->Subkind == 4 || C->Subkind == 7))) { Bad(V); return; }
    Terminal(V, *C, 1, 2, 0, Location, true);
}
C2_BODY void varest_ws_c2l_debit_named1(const VrLwsCustody1* V, const VrLwsRowRef1* Ref,
    const VrLwsBudgetRef1* B, uint32_t Operation, VrLwsHookOutcome1* Out)
{
    using namespace C2LSourceDetail;
    const FResourceBinding1* R = View(V);
    if (!R || !Output(R, Out) || !External(R, Ref, sizeof(*Ref), 8)
        || !C2LSourceDetail::Separate(Out, sizeof(*Out), Ref, sizeof(*Ref)) || !BudgetExternal(R, B, Out, sizeof(*Out))
        || !BudgetExternal(R, B, Ref, sizeof(*Ref))) return;
    *Out = {VR_LWS_HOOK_LOCAL_INVALID1, 0, 0, 0};
    if (!Row(R, Ref) || Operation < VR_LWS_NAMED_ACQUIRE1 || Operation > VR_LWS_NAMED_TLS_SETUP1) return;
    const FResourceCell1& C = R->Journal->Cells[Ref->Cell];
    const bool Known = C.LocalState == ResourceObtainedPending1 || C.LocalState == ResourceOwnedAck1;
    if (Operation != VR_LWS_NAMED_DISPOSE1 && R->Journal->Header.Fault)
    { Out->Disposition = ResourceProtocolFailure1; return; }
    if (Operation == VR_LWS_NAMED_ACQUIRE1 ? C.LocalState != ResourceReserved1 : !Known) return;
    if (Operation == VR_LWS_NAMED_RNG1 && !((C.Kind == 7 && C.Subkind == 1) || (C.Kind == 8 && C.Subkind == 1))) return;
    if (Operation == VR_LWS_NAMED_ENGINE_ROOTS1
        || (Operation == VR_LWS_NAMED_TLS_SETUP1 && B->Creating))
    {
        // One OUTER Engine/setup call only; internal refs/work are provider H.
        // Keep all pending disposal/ACK credits inside the same startup phase.
        if (B->Active || !B->Creating || B->Creating->Phase != VR_LWS_PHASE_STARTUP1
            || C.Kind != 8 || C.Subkind != 2) return;
        const uint32_t Required = RequiredGas(R);
        VrLwsPhaseBudget1& P = *B->Creating;
        if (!P.Remaining || P.Remaining - 1 < Required)
        { Out->Disposition = ResourceQuotaDeferred1; return; }
        --P.Remaining; ++P.Used; Out->Disposition = ResourceCompleted1;
        return; // ROOTS10 remains creating-only; active setup11 is checked below.
    }
    if (Operation == VR_LWS_NAMED_TLS_SETUP1)
    {
        if (!B->Active || !C.NativeSerial || !C.NativeValue || C.Kind != 8 || C.Subkind != 3) return;
    }
    else if (Operation >= VR_LWS_NAMED_CONNECT1)
    {
        const bool ClientFd = C.Kind == 5 && C.Subkind == 3;
        const bool WakeRead = Operation == VR_LWS_NAMED_READ1 && C.Kind == 9 && C.Subkind == 1;
        const bool ContextPoll = Operation == VR_LWS_NAMED_POLL1 && C.Kind == 7 && C.Subkind == 1;
        if (!B->Active || (Operation == VR_LWS_NAMED_POLL1 ? !ContextPoll : (!ClientFd && !WakeRead))
            || (Operation != VR_LWS_NAMED_POLL1 && C.NativeValue > INT32_MAX)) return;
    }
    const auto Domain = D::Domain(C.Kind, C.Subkind);
    if (Operation == VR_LWS_NAMED_DISPOSE1 && (Domain == D::Init2 || Domain == D::Index2 || Domain == D::Store2)) return;
    bool PipeAcquire = Operation == VR_LWS_NAMED_ACQUIRE1 && C.Kind == 9;
    if (PipeAcquire)
    {
        if (C.Subkind != 1) return; // ONE pipe2 debit, selected by its read-side row only.
        bool WriteReserved = false;
        for (uint32_t I = 12; I < 140; ++I)
        {
            const FResourceCell1& W = R->Journal->Cells[I];
            if (W.Kind == 9 && W.Subkind == 2 && W.ParentSerial == C.ParentSerial
                && W.LocalState == ResourceReserved1 && W.NativeSerial && W.SourceSerial) WriteReserved = true;
        }
        if (!WriteReserved) { Out->Disposition = ResourceParentDeferred1; return; }
    }
    if (B->Active) { Outcome(*Out, ResourceDebitBorrowedNamed1(B->Active, R, Ref->Cell, Operation)); return; }
    VrLwsPhaseBudget1& P = *B->Creating;
    const uint32_t Required = RequiredGas(R);
    const uint32_t Retain = Operation == VR_LWS_NAMED_ACQUIRE1 ? Required + (PipeAcquire ? 6u : 3u)
        : (Operation == VR_LWS_NAMED_DISPOSE1 && Required ? Required - 1 : Required);
    if (!P.Remaining || P.Remaining - 1 < Retain) { Out->Disposition = ResourceQuotaDeferred1; return; }
    --P.Remaining; ++P.Used; Out->Disposition = ResourceCompleted1;
}
C2_BODY void varest_ws_c2l_retire_source_row1(const VrLwsCustody1* V, const VrLwsRowRef1* Ref, VrLwsHookOutcome1* Out)
{
    using namespace C2LSourceDetail;
    const FResourceBinding1* R = View(V);
    if (!R || !Output(R, Out) || !External(R, Ref, sizeof(*Ref), 8)
        || !C2LSourceDetail::Separate(Ref, sizeof(*Ref), Out, sizeof(*Out))) return;
    *Out = {VR_LWS_HOOK_LOCAL_INVALID1, 0, 0, 0}; if (!Row(R, Ref)) return;
    FResourceCell1& C = R->Journal->Cells[Ref->Cell];
    if (!D::Terminal(C.LocalState)) return;
    for (uint32_t I = 0; I < 140; ++I)
    {
        if (I == Ref->Cell) continue;
        const FResourceCell1& Other = R->Journal->Cells[I];
        if (Other.LocalState != ResourceEmpty1 && (Other.ParentSerial == C.NativeSerial
            || (I >= 12 && Other.SourceSerial == C.SourceSerial))) { Out->Disposition = ResourceParentDeferred1; return; }
    }
    // Genuine source caller has retired ALL physical/stack/callback references.
    // Neither terminal ACK nor this local predicate establishes that premise.
    C = {}; Out->Disposition = ResourceCompleted1;
    // Native rows/NextSerial, Control.NextSourceSerial and all Header history stay untouched.
}
}

namespace VaRest::WebSocket::Lws
{
C2_BODY FNativeOwnerResources1::FNativeOwnerResources1(void* Owner, uint64_t OwnerEpoch, uint64_t ServiceEpoch,
    C2ResourcesC1::FResourceEntryAddress1 Reserve, C2ResourcesC1::FResourceEntryAddress1 Obtained,
    C2ResourcesC1::FResourceEntryAddress1 NotObtained, C2ResourcesC1::FResourceEntryAddress1 Closed,
    C2ResourcesC1::FResourceEntryAddress1 Uncertain)
    : Journal{}, Storage{48, 1, nullptr, 0, nullptr, 0, 0, 0},
      Binding{96, 1, Owner, OwnerEpoch, ServiceEpoch, Reserve, Obtained, NotObtained, Closed, Uncertain, 2, 140, &Journal, 0}, Control{}, View{&Binding, &Control}
{
    static_assert(sizeof(FNativeOwnerResources1) == 9248 && alignof(FNativeOwnerResources1) == 8);
    static_assert(std::is_standard_layout_v<FNativeOwnerResources1>);
    static_assert(offsetof(FNativeOwnerResources1, Journal) == 0 && offsetof(FNativeOwnerResources1, Storage) == 9024
        && offsetof(FNativeOwnerResources1, Binding) == 9072
        && offsetof(FNativeOwnerResources1, Control) == 9168 && offsetof(FNativeOwnerResources1, View) == 9232);
    Journal.Header.Size = 64; Journal.Header.Version = 1; Journal.Header.Profile = 2; Journal.Header.CellCount = 140;
}
C2_BODY const C2ResourcesC1::FResourceBinding1* FNativeOwnerResources1::GetBinding1() const { return &Binding; }
C2_BODY C2ResourcesC1::FStorageJournal1* FNativeOwnerResources1::GetStorage1() { return &Storage; }
C2_BODY C2ResourcesC1::FResourceProgress1 FNativeOwnerResources1::ReconcileAfterC1ReturnOnCreatingThread(VrLwsPhaseBudget1* Phase)
{
    using namespace C2ResourcesC1;
    FResourceProgress1 Out{32, 1, 0, 0, 0, 0, 0, 0};
    if (!C2ResourceDetail::Shape(&Binding) || !C2LSourceDetail::Phase(&Binding, Phase))
    { Out.Fault = ResourceProtocolFault1; return Out; }
    const VrLwsBudgetRef1 Budget{nullptr, Phase};
    uint32_t Left = 280; const uint32_t Start = Journal.Header.ReconcileCursor;
    for (uint32_t K = 0; K < 140; ++K)
    {
        const uint32_t I = (Start + K) % 140; ++Out.VisitedRows;
        const uint32_t State = Journal.Cells[I].LocalState;
        if (!C2ResourceDetail::Pending(State) || State == ResourceReserved1) continue;
        if (Left < 2) continue;
        const uint64_t Before = Journal.Header.HookDispatches;
        const uint64_t IdentityBefore = Journal.Header.NativeIdentityCalls; Left -= 2;
        const FHookOutcome1 O = C2LSourceDetail::Dispatch(&Binding, &Budget, nullptr, I);
        const bool Dispatched = Journal.Header.HookDispatches != Before;
        const uint32_t Cost = Dispatched ? (O.Disposition == ResourceProtocolFailure1 ? 2u : O.NativeCalls) : 0u;
        Left += 2 - Cost;
        if (Dispatched) ++Out.Dispatches;
        Out.NativeCalls += static_cast<uint32_t>(Journal.Header.NativeIdentityCalls - IdentityBefore);
        // Left retains worst2 on a malformed reply; this output counts only
        // validated reported observations. Fault marks an incomplete diagnostic.
    }
    Journal.Header.ReconcileCursor = (Start + 1) % 140;
    for (uint32_t I = 0; I < 140; ++I)
    {
        const FResourceCell1& C = Journal.Cells[I];
        Out.PendingRows += C2ResourceDetail::Pending(C.LocalState) ? 1u : 0u;
        // Pool terminal ACK still retains historical/source-reference custody.
        // Only genuine retire_source_row consumes that proof and clears the row.
        Out.OutstandingRows += C.LocalState != ResourceEmpty1 && (I >= 12 || !C2ResourceDetail::Terminal(C.LocalState)) ? 1u : 0u;
    }
    Out.Fault = Journal.Header.Fault; return Out;
}
}
#undef C2_BODY
#endif
