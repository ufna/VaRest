// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "VaRestNumericConnectorNative.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
#include <new>
#include <type_traits>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
// Use the same OpenSSL UI_ST spelling as VaRestLwsInclude.h; keep the macro local.
#pragma push_macro("UI")
#undef UI
#define UI UI_ST
#include <openssl/rand.h>
#pragma pop_macro("UI")

#define C1_BODY [[gnu::used, gnu::visibility("hidden")]]

namespace VaRest::WebSocket::NumericConnectorC1
{
namespace
{
    enum : uint32_t { Ready = 1, Service = 2, Finalized = 3, NeverStartedFinalized = 4 };
    enum : uint32_t { Absent = 0, ReservedFd = 1, Owned = 2, Closing = 3, Uncertain = 4, Closed = 5 };
    enum : uint32_t { ListenerKind = 1, AcceptedKind = 2, UpstreamKind = 3 };
    enum : uint32_t { JournalAbsent = 0, Obtained = 1, Live = 2, LivePoisoned = 3, Releasing = 4, JournalUncertain = 5 };
    enum : uint32_t { TurnClosed = 0, Before = 1, Between = 2, After = 3, CleanupTurn = 4 };
    enum : uint32_t { Free = 0, Unbound = 1, Bound = 2, NeedSocket = 3, SetupLinger = 4,
        SetupSend = 5, SetupReceive = 6, QuerySend = 7, QueryReceive = 8, Connect = 9,
        Connecting = 10, CheckPeer = 11, ProxyBuild = 12, ProxySend = 13,
        ProxyRequestClear = 14, ProxyReceive = 15, ProxySuffix = 16,
        ProxyResponseClear = 17, Reply = 18, Tunnel = 19, TerminalReply = 20,
        Retiring = 21, NativeClosed = 22, RetryReady = 23, Established = 24 };
    enum : uint32_t { PendingFree = 0, PendingLinger = 1, PendingSend = 2, PendingReceive = 3,
        PendingQuerySend = 4, PendingQueryReceive = 5, PendingLocal = 6, PendingRemote = 7,
        PendingHeader = 8, PendingParsed = 9, PendingClosing = 10, PendingClear = 11 };
    enum : uint32_t { EofLocal = 1u, EofUpstream = 2u, ShutUpstream = 4u, ShutLocal = 8u,
        Claimed = 16u, RetryConnect = 32u, WsiRetired = 64u, Cancelled = 128u,
        PairClosedRecorded = 256u, ConnectIssued = 512u, ConnectedPeer = 1024u,
        TerminalStatus = 2048u, HeaderFromUpstream = 4096u };
    enum : uint32_t { KnownFDs, UncertainFDs, PeakFDs, PendingCount, AcceptedFDs,
        UpstreamFDs, PairRecords, UpstreamCreated, Accepts, Denied, Authorized,
        DirectCalls, Turns, LastCalls, LastBytes, BytesRead, BytesWritten, ClosedFDs,
        ProtocolRejects, DeadlineExpiries, Peak00, Peak01, Peak10, Peak11,
        WouldBlock, SuppressedFull, Canceled, PairsClosed, StartupCalls, StateSequence };
    enum : uint32_t { InvariantPoison = 1, ClockPoison = 2, NativePoison = 3, CounterPoison = 4 };
    constexpr uint64_t Magic1 = UINT64_C(0x564e554d434f5231);
    constexpr uint64_t Ns250ms = UINT64_C(250000000), Ns2s = UINT64_C(2000000000);

    struct FHeader
    {
        uint64_t Magic; uint32_t Version, LogicalBytes;
        uint64_t MappingBytes, CreatorProcess;
        pthread_t CreatorThread, ServiceThread;
        uint32_t Stop, OwnershipPhase, Poison, Reserved;
    };
    struct FFd
    {
        int32_t Fd; uint32_t Custody; uint64_t Serial;
        int32_t NativeError; uint32_t Kind; uint64_t Reserved;
    };
    struct FTurn
    {
        uint64_t Generation, StartedNs, NowNs;
        uint32_t Phase, CallsLeft, BytesLeft, AcceptsLeft, FirstSlot, FirstPending;
        uint32_t DirectionBytesLeft[2][2], ProgressCursor, PendingCursor, UsedCalls, UsedBytes;
        uint64_t Reserved[6];
    };
    struct FPending
    {
        FFd Fd; uint64_t Serial, IngressDeadline; FTuple1 Tuple;
        uint32_t HeaderBytes, ParseState, MatchedSlot, Reserved;
        uint64_t ClaimGeneration; uint32_t HeaderEnd, ReservedTail;
    };
    struct FRing { uint32_t Head, Used, Peak, Reserved; };
    struct FSlot
    {
        uint64_t NextGeneration, Generation, Conn, Route, WsiToken, PhaseDeadline, HalfCloseDeadline, IngressSerial;
        uint32_t Phase, SelectedCandidate, Flags, HeaderBytes, HeaderSent, LocalReplyBytes, LocalReplySent, ProxyStatus;
        FRing Rings[2]; FEndpoint1 ActualPeer; uint32_t Reserved; FTuple1 Tuple;
        FAttempt1 Attempt; FFd Local, Upstream;
        char ExpectedBase64[97], ProxyAddress[81]; uint8_t ReservedTail[6]; char LocalReply[64];
    };
}

struct FNative1
{
    FHeader Header; FFd Listener; FTurn Turn; FStats1 Stats;
    FPending Pending[4]; FSlot Slots[2];
    uint8_t RingPayload[2][2][16384];
    uint8_t PendingHeader[4][4096];
    uint8_t SlotHeader[2][4096];
};

namespace
{
    static_assert(sizeof(void*) == 8 && sizeof(pthread_t) == 8 && alignof(pthread_t) <= 8);
    static_assert(sizeof(int) == sizeof(int32_t) && sizeof(uintptr_t) == sizeof(uint64_t));
    static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);
    static_assert(__atomic_always_lock_free(4, nullptr) && __atomic_always_lock_free(8, nullptr));
#define C1_LAYOUT(T, N, A) static_assert(sizeof(T) == N && alignof(T) == A); \
    static_assert(std::is_trivial_v<T> && std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>)
    C1_LAYOUT(FEndpoint1, 28, 4); C1_LAYOUT(FTuple1, 56, 4); C1_LAYOUT(FAttempt1, 1304, 8);
    C1_LAYOUT(FReservation1, 32, 8); C1_LAYOUT(FBinding1, 40, 8); C1_LAYOUT(FClaim1, 104, 8);
    C1_LAYOUT(FLocalRouteSecret1, 112, 4); C1_LAYOUT(FStats1, 256, 8); C1_LAYOUT(FStorageJournal1, 48, 8);
    C1_LAYOUT(FHeader, 64, 8); C1_LAYOUT(FFd, 32, 8); C1_LAYOUT(FTurn, 128, 8);
    C1_LAYOUT(FPending, 136, 8); C1_LAYOUT(FRing, 16, 4); C1_LAYOUT(FSlot, 1832, 8);
    C1_LAYOUT(FNative1, 94800, 8);
    static_assert(sizeof(FNative1) <= 131072 && std::is_aggregate_v<FNative1> && std::is_trivially_destructible_v<FNative1>);
#define C1_OFFSET(T, F, N) static_assert(offsetof(T, F) == N)
    C1_OFFSET(FHeader, Stop, 48); C1_OFFSET(FHeader, OwnershipPhase, 52);
    C1_OFFSET(FNative1, Listener, 64); C1_OFFSET(FNative1, Turn, 96); C1_OFFSET(FNative1, Stats, 224);
    C1_OFFSET(FNative1, Pending, 480); C1_OFFSET(FNative1, Slots, 1024); C1_OFFSET(FNative1, RingPayload, 4688);
    C1_OFFSET(FNative1, PendingHeader, 70224); C1_OFFSET(FNative1, SlotHeader, 86608);
    C1_OFFSET(FAttempt1, Route, 8); C1_OFFSET(FAttempt1, OriginalDeadlineNs, 16);
    C1_OFFSET(FAttempt1, Mode, 24); C1_OFFSET(FAttempt1, Candidates, 32);
    C1_OFFSET(FAttempt1, AuthorityBytes, 256); C1_OFFSET(FAttempt1, UpstreamAuthBytes, 260);
    C1_OFFSET(FAttempt1, Authority, 272); C1_OFFSET(FAttempt1, UpstreamAuth, 785); C1_OFFSET(FAttempt1, ReservedTail, 1298);
    C1_OFFSET(FSlot, Phase, 64); C1_OFFSET(FSlot, Rings, 96); C1_OFFSET(FSlot, ActualPeer, 128);
    C1_OFFSET(FSlot, Tuple, 160); C1_OFFSET(FSlot, Attempt, 216); C1_OFFSET(FSlot, Local, 1520);
    C1_OFFSET(FSlot, Upstream, 1552); C1_OFFSET(FSlot, ExpectedBase64, 1584);
    C1_OFFSET(FSlot, ProxyAddress, 1681); C1_OFFSET(FSlot, LocalReply, 1768);
    C1_OFFSET(FPending, Serial, 32); C1_OFFSET(FPending, Tuple, 48); C1_OFFSET(FPending, HeaderBytes, 104);
    C1_OFFSET(FPending, ClaimGeneration, 120); C1_OFFSET(FStorageJournal1, Raw, 8);
    C1_OFFSET(FStorageJournal1, Object, 24); C1_OFFSET(FStorageJournal1, State, 32);
#undef C1_OFFSET
#undef C1_LAYOUT

    static void Zero(void* To, uint32_t N)
    {
        volatile uint8_t* B = static_cast<volatile uint8_t*>(To);
        for (uint32_t I = 0; I < N; ++I) B[I] = 0;
    }
    static void Copy(const void* From, void* To, uint32_t N)
    {
        const uint8_t* A = static_cast<const uint8_t*>(From); uint8_t* B = static_cast<uint8_t*>(To);
        for (uint32_t I = 0; I < N; ++I) B[I] = A[I];
    }
    static bool AllZero(const void* From, uint32_t N)
    {
        const uint8_t* B = static_cast<const uint8_t*>(From);
        for (uint32_t I = 0; I < N; ++I) if (B[I] != 0) return false;
        return true;
    }
    template<class T> static bool Aligned(const T* P) { return P != nullptr && reinterpret_cast<uintptr_t>(P) % alignof(T) == 0; }
    static bool Separate(const void* A, uint64_t N, const void* B, uint64_t M)
    {
        if (A == nullptr || B == nullptr) return true; // Input validity is checked separately; NULL cannot overlap.
        const uintptr_t X = reinterpret_cast<uintptr_t>(A), Y = reinterpret_cast<uintptr_t>(B);
        if (N > UINTPTR_MAX - X || M > UINTPTR_MAX - Y) return false;
        return X + N <= Y || Y + M <= X;
    }
    static bool MappingExternalSpan(const FNative1* S, const void* P, uint64_t Bytes)
    {
        if (S == nullptr) return true;
        // Real accessible S is a caller premise. The rounded tail belongs to
        // this mapping too; outputs/ingress may not alias that tail.
        const uint64_t Extent = S->Header.MappingBytes;
        return Extent >= sizeof(FNative1) && Extent <= 131072 && Separate(S, Extent, P, Bytes);
    }
    template<class T> static bool External(const FNative1* S, const T* P)
    { return Aligned(P) && MappingExternalSpan(S, P, sizeof(T)); }
    template<class T> static bool Output(T* P)
    {
        if (!Aligned(P) || P->Size != sizeof(T) || P->Version != Version1) return false;
        Zero(P, sizeof(T)); P->Size = sizeof(T); P->Version = Version1; return true;
    }
    static uint32_t Load(const uint32_t& V) { return __atomic_load_n(&V, __ATOMIC_ACQUIRE); }
    static void Store(uint32_t& V, uint32_t N) { __atomic_store_n(&V, N, __ATOMIC_RELEASE); }
    static bool Object(const FNative1* S)
    {
        // Actual accessible live storage/code protection is a caller premise, not probed here.
        return Aligned(S) && S->Header.Magic == Magic1 && S->Header.Version == Version1
            && S->Header.LogicalBytes == sizeof(FNative1) && S->Header.MappingBytes >= sizeof(FNative1)
            && S->Header.MappingBytes <= 131072 && S->Header.Reserved == 0;
    }
    static void Poison(FNative1& S, uint32_t Why)
    {
        if (S.Header.Poison == 0) S.Header.Poison = Why;
        Store(S.Header.Stop, 1);
    }
    static bool Add(FNative1& S, uint32_t Index, uint64_t Amount = 1)
    {
        uint64_t& Counter = S.Stats.Counters[Index];
        const uint64_t Old = __atomic_load_n(&Counter, __ATOMIC_RELAXED);
        if (Old > UINT64_MAX - Amount) { Poison(S, CounterPoison); return false; }
        uint64_t Expected = Old;
        if (!__atomic_compare_exchange_n(&Counter, &Expected, Old + Amount, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        { Poison(S, InvariantPoison); return false; }
        return true;
    }
    static void SetCounter(FNative1& S, uint32_t Index, uint64_t Value)
    { __atomic_store_n(&S.Stats.Counters[Index], Value, __ATOMIC_RELAXED); }
    static uint64_t Counter(const FNative1& S, uint32_t Index)
    { return __atomic_load_n(&S.Stats.Counters[Index], __ATOMIC_RELAXED); }
    static bool Serial(FNative1& S, uint64_t& Out)
    {
        if (!Add(S, StateSequence)) return false;
        Out = Counter(S, StateSequence); return true;
    }
    static bool Known(const FFd& F) { return F.Custody == Owned; }
    static bool Discharged(const FFd& F) { return F.Custody == Absent || F.Custody == Closed; }
    static uint32_t NonlistenerKnown(const FNative1& S)
    {
        uint32_t N = 0;
        for (const FPending& P : S.Pending) N += Known(P.Fd) ? 1u : 0u;
        for (const FSlot& P : S.Slots) { N += Known(P.Local) ? 1u : 0u; N += Known(P.Upstream) ? 1u : 0u; }
        return N;
    }
    static void Refresh(FNative1& S)
    {
        uint32_t K = 0, U = 0, A = 0, Up = 0, P = 0, Pairs = 0;
        const auto Cell = [&](const FFd& F)
        {
            K += F.Custody == Owned ? 1u : 0u; U += F.Custody == Uncertain ? 1u : 0u;
            if (F.Custody == Owned || F.Custody == Uncertain)
            { A += F.Kind == AcceptedKind ? 1u : 0u; Up += F.Kind == UpstreamKind ? 1u : 0u; }
        };
        Cell(S.Listener);
        for (const FPending& V : S.Pending) { Cell(V.Fd); P += V.ParseState != PendingFree ? 1u : 0u; }
        for (const FSlot& V : S.Slots)
        {
            Cell(V.Local); Cell(V.Upstream);
            Pairs += (V.Flags & Claimed) != 0 && (V.Flags & PairClosedRecorded) == 0 ? 1u : 0u;
        }
        SetCounter(S, KnownFDs, K); SetCounter(S, UncertainFDs, U); SetCounter(S, PendingCount, P);
        SetCounter(S, AcceptedFDs, A); SetCounter(S, UpstreamFDs, Up); SetCounter(S, PairRecords, Pairs);
        if (K + U > Counter(S, PeakFDs)) SetCounter(S, PeakFDs, K + U);
        if (K + U > 9 || A > 6 || Up > 2 || P > 4 || Pairs > 2) Poison(S, InvariantPoison);
    }
    static void OutsideCall(FNative1& S, bool Startup = false)
    {
        // Lifecycle/identity imports outside an ordinary turn are explicit, never free calls.
        Add(S, DirectCalls); if (Startup) Add(S, StartupCalls);
    }
    static bool TurnActive(const FNative1& S) { return S.Turn.Phase >= Before && S.Turn.Phase <= CleanupTurn; }
    static bool WorkCall(FNative1& S, const FResourceBinding1* Binding, uint32_t NewFd = 0)
    {
        if (Binding->Journal->Header.Fault) { Poison(S, NativePoison); return false; }
        if (!TurnActive(S) || S.Turn.Phase == CleanupTurn || S.Turn.CallsLeft == 0) return false;
        const uint32_t RequiredCloses = NonlistenerKnown(S) + ResourceClientCloseCredits1(Binding) + NewFd;
        if (S.Turn.CallsLeft - 1 < RequiredCloses) return false;
        --S.Turn.CallsLeft; ++S.Turn.UsedCalls; return Add(S, DirectCalls);
    }
    static bool CloseCall(FNative1& S, const FResourceBinding1* Binding)
    {
        if (!TurnActive(S) || S.Turn.CallsLeft == 0
            || S.Turn.CallsLeft < NonlistenerKnown(S) + ResourceClientCloseCredits1(Binding))
        { Poison(S, InvariantPoison); return false; }
        --S.Turn.CallsLeft; ++S.Turn.UsedCalls; Add(S, DirectCalls); return true;
    }
    // Local association/physical membership, never owner authority. All spans and
    // qualified code are accessible and retained by the real creating graph.
    static bool ResourceKnownTerminal(uint32_t State)
    { return State == ResourceClosedAck1 || State == ResourceNotObtainedAck1; }
    static bool ResourceHasEvent(uint32_t State)
    { return State == ResourceObtainedPending1 || (State >= ResourceNotObtainedPending1 && State <= ResourceObtainedThenUncertain1) || State == ResourceAcquireFaultPending2; }
    static bool ResourceShape(const FResourceBinding1* R, const FStorageJournal1* Storage = nullptr)
    {
        return ResourceBindingValid1(R) && (!Storage || reinterpret_cast<uintptr_t>(Storage)
            == reinterpret_cast<uintptr_t>(R->Journal) + 9024);
    }
    static const FStorageJournal1* ResourceStorage(const FResourceBinding1* R)
    { return reinterpret_cast<const FStorageJournal1*>(reinterpret_cast<const uint8_t*>(R->Journal) + 9024); }
    static bool ResourceExternal(const FResourceBinding1* R, const void* P, uint64_t Bytes)
    { return Separate(R->Journal, 9248, P, Bytes); }
    static bool Resources(const FNative1* S, const FResourceBinding1* R)
    {
        if (!ResourceShape(R) || !Object(S) || R->Journal->Header.CreateEntered != 1) return false;
        const FStorageJournal1& J = *ResourceStorage(R); const FResourceCell1& C = R->Journal->Cells[0];
        return J.Size == 48 && J.Version == 1 && J.Reserved == 0 && J.Object == S && J.Raw == S
            && (J.State == Live || J.State == LivePoisoned)
            && J.MappingBytes == S->Header.MappingBytes && Separate(R->Journal, 9248, J.Raw, J.MappingBytes)
            && C.NativeValue == reinterpret_cast<uintptr_t>(J.Raw) && C.Extent == J.MappingBytes
            && C.SourceSerial == 0 && C.SourceLocation == 0
            && (C.LocalState == ResourceObtainedPending1 || C.LocalState == ResourceOwnedAck1);
    }
    static FHookOutcome1 ResourceDeferred()
    { return {ResourceQuotaDeferred1, 0, 0, 0}; }
    static uint32_t ResourceDispatchCost(const FHookOutcome1& O, bool Dispatched)
    {
        // Invalid reply retains worst2, never advertises it as actual native data.
        return Dispatched ? (O.Disposition == ResourceProtocolFailure1 ? 2u : O.NativeCalls) : 0u;
    }
    static FHookOutcome1 ResourceDispatch(FNative1* S, const FResourceBinding1* R,
        const FResourceSpec1* Spec, uint32_t Cell, bool Startup = false, uint64_t* Prefix = nullptr)
    {
        const bool ActiveTurn = S && TurnActive(*S);
        if (ActiveTurn)
        {
            if (S->Turn.CallsLeft < 2 || S->Turn.CallsLeft - 2 < NonlistenerKnown(*S) + ResourceClientCloseCredits1(R)) return ResourceDeferred();
            S->Turn.CallsLeft -= 2;
        }
        const uint64_t Before = R->Journal->Header.HookDispatches;
        const FHookOutcome1 O = Spec ? ResourceReserve1(R, Spec) : ResourceAcknowledgeNext1(R, Cell);
        const uint32_t Cost = ResourceDispatchCost(O, R->Journal->Header.HookDispatches != Before);
        if (ActiveTurn) { S->Turn.CallsLeft += 2 - Cost; S->Turn.UsedCalls += Cost; }
        if (S) { Add(*S, DirectCalls, Cost); if (Startup) Add(*S, StartupCalls, Cost); }
        if (Prefix) *Prefix += Cost;
        if (S && R->Journal->Header.Fault != 0) Poison(*S, NativePoison);
        return O;
    }
    static EStatus1 ResourceStatus(const FHookOutcome1& O)
    {
        if (O.Disposition == ResourceCompleted1) return EStatus1::OK;
        if (O.Disposition == ResourceNativeBusy1 || O.Disposition == ResourceParentDeferred1 || O.Disposition == ResourceQuotaDeferred1) return EStatus1::PENDING;
        return O.Disposition == ResourceNativeRefused1 ? EStatus1::NATIVE_ERROR : EStatus1::RETAINED_POISON;
    }
    static FResourceSpec1 ResourceSpec(uint32_t Cell, uint64_t Parent = 0)
    { return {Cell == 0 ? 4u : (Cell == 1 ? 1u : (Cell < 8 ? 2u : 3u)), Cell >= 10 ? 2u : 1u, Cell, Cell, Parent}; }
    static bool ResourceReserveCell(FNative1& S, const FResourceBinding1* R, uint32_t Cell, uint64_t Parent = 0, bool Startup = false)
    {
        if (R->Journal->Header.Fault || Load(S.Header.Stop)) return false;
        const FResourceSpec1 Spec = ResourceSpec(Cell, Parent);
        return ResourceDispatch(&S, R, &Spec, Cell, Startup).Disposition == ResourceCompleted1;
    }
    static void ResourceReconcile(FNative1& S, const FResourceBinding1* R)
    {
        const uint32_t Start = R->Journal->Header.ReconcileCursor;
        for (uint32_t K = 0; K < 140; ++K)
        {
            const uint32_t I = (Start + K) % 140;
            if (ResourceHasEvent(R->Journal->Cells[I].LocalState)) (void)ResourceDispatch(&S, R, nullptr, I);
        }
        R->Journal->Header.ReconcileCursor = (Start + 1) % 140;
    }
    static uint32_t ResourceFdCell(const FNative1& S, const FResourceBinding1* R, const FFd& F)
    {
        uint32_t Location = UINT32_MAX;
        if (&F == &S.Listener) Location = 1;
        for (uint32_t I = 0; I < 4; ++I) if (&F == &S.Pending[I].Fd) Location = 2 + I;
        for (uint32_t I = 0; I < 2; ++I)
        { if (&F == &S.Slots[I].Local) Location = 6 + I; if (&F == &S.Slots[I].Upstream) Location = 10 + I; }
        if (Location == UINT32_MAX) return 12;
        const uint32_t First = Location == 1 ? 1u : (Location < 8 ? 2u : Location);
        const uint32_t End = Location == 1 ? 2u : (Location < 8 ? 8u : Location + 1);
        for (uint32_t I = First; I < End; ++I)
        {
            const FResourceCell1& C = R->Journal->Cells[I];
            if (C.NativeSerial && C.SourceSerial == F.Serial && C.SourceLocation == Location) return I;
        }
        return 12;
    }
    static bool ResourceSourceHeld(const FNative1& S, const FResourceCell1& C)
    {
        if (!C.SourceSerial) return false;
        for (const FPending& P : S.Pending)
            if (P.ParseState != PendingFree && P.Serial == C.SourceSerial) return true;
        for (const FSlot& V : S.Slots)
            if (V.Phase != Free && V.IngressSerial == C.SourceSerial) return true;
        return false;
    }
    static bool ResourcePendingCanClear(const FNative1& S, const FResourceBinding1* R, uint32_t Index)
    {
        const FPending& P = S.Pending[Index];
        if (P.Fd.Custody == Absent) return true;
        const uint32_t Cell = ResourceFdCell(S, R, P.Fd);
        if (Cell < 12) return ResourceKnownTerminal(R->Journal->Cells[Cell].LocalState);
        for (uint32_t I = 0; I < 2; ++I)
        {
            const FSlot& V = S.Slots[I];
            if (V.Phase == Free || V.IngressSerial != P.Serial || V.Local.Serial != P.Serial) continue;
            for (uint32_t J = 2; J < 8; ++J)
            {
                const FResourceCell1& C = R->Journal->Cells[J];
                if (C.NativeSerial && C.SourceSerial == P.Serial && C.SourceLocation == 6 + I
                    && C.LocalState != ResourceReserved1 && C.LocalState != ResourceEmpty1) return true;
            }
        }
        return false;
    }
    static uint32_t ResourceFreeIngress(FNative1& S, const FResourceBinding1* R)
    {
        for (uint32_t I = 2; I < 8; ++I)
        {
            const FResourceCell1& C = R->Journal->Cells[I];
            if (C.LocalState == ResourceEmpty1) return I;
            if (ResourceKnownTerminal(C.LocalState) && !ResourceSourceHeld(S, C))
            {
                // Consume ONLY this acknowledged historical physical association
                // before its row can be overwritten. Failed/no-attempt Free cells
                // still wait for their actual terminal ACK; newer serials survive.
                for (FPending& P : S.Pending)
                    if (P.ParseState == PendingFree && P.Fd.Custody == Closed
                        && P.Fd.Fd == -1 && P.Fd.Kind == AcceptedKind && P.Fd.Serial != 0
                        && P.Fd.Serial == C.SourceSerial)
                        P.Fd = FFd{-1, Absent, 0, 0, AcceptedKind, 0};
                return I;
            }
        }
        return 12;
    }
    static bool ResourceSlotTerminal(const FNative1& S, const FResourceBinding1* R, uint32_t I)
    {
        if (R->Journal->Header.Fault) return false;
        const FSlot& V = S.Slots[I];
        const FResourceCell1& Parent = R->Journal->Cells[8 + I];
        const FResourceCell1& Child = R->Journal->Cells[10 + I];
        if ((V.Flags & Claimed) && (!ResourceKnownTerminal(Parent.LocalState)
            || Parent.SourceSerial != V.Generation || Parent.NativeValue != V.Generation)) return false;
        if (Parent.LocalState != ResourceEmpty1 && !ResourceKnownTerminal(Parent.LocalState)) return false;
        if (Child.LocalState != ResourceEmpty1 && !ResourceKnownTerminal(Child.LocalState)) return false;
        if (V.IngressSerial)
        {
            for (uint32_t J = 2; J < 8; ++J)
                if (R->Journal->Cells[J].SourceSerial == V.IngressSerial && R->Journal->Cells[J].SourceLocation == 6 + I)
                    return ResourceKnownTerminal(R->Journal->Cells[J].LocalState);
            return false;
        }
        return true;
    }
    static uint32_t ByteRoom(const FNative1& S, uint32_t Slot, uint32_t Direction)
    {
        return S.Turn.BytesLeft < S.Turn.DirectionBytesLeft[Slot][Direction]
            ? S.Turn.BytesLeft : S.Turn.DirectionBytesLeft[Slot][Direction];
    }
    static bool Bytes(FNative1& S, uint32_t Slot, uint32_t Direction, uint32_t N)
    {
        if (N > ByteRoom(S, Slot, Direction)) return false;
        S.Turn.BytesLeft -= N; S.Turn.DirectionBytesLeft[Slot][Direction] -= N; S.Turn.UsedBytes += N; return true;
    }
    static EStatus1 Active(FNative1* S)
    {
        if (!Object(S)) return EStatus1::INVALID;
        if (Load(S->Header.OwnershipPhase) != Service || S->Turn.Phase < Before || S->Turn.Phase > After) return EStatus1::WRONG_ROLE;
        return EStatus1::OK;
    }
    static bool Now(FNative1& S, const FResourceBinding1* Binding, uint64_t& Out)
    {
        if (!WorkCall(S, Binding)) return false;
        timespec Time {};
        const int R = ::clock_gettime(CLOCK_MONOTONIC, &Time);
        const int Error = R != 0 ? errno : 0;
        if (R != 0 || Time.tv_sec < 0 || Time.tv_nsec < 0 || Time.tv_nsec >= 1000000000)
        { (void)Error; Poison(S, ClockPoison); return false; }
        const uint64_t Sec = static_cast<uint64_t>(Time.tv_sec), Ns = static_cast<uint64_t>(Time.tv_nsec);
        if (Sec > (UINT64_MAX - Ns) / UINT64_C(1000000000)) { Poison(S, ClockPoison); return false; }
        Out = Sec * UINT64_C(1000000000) + Ns;
        if (Out < S.Turn.NowNs) { Poison(S, ClockPoison); return false; }
        S.Turn.NowNs = Out; return true;
    }
    static uint64_t Deadline(uint64_t At, uint64_t Interval, uint64_t Original)
    {
        if (At >= Original) return Original;
        const uint64_t Left = Original - At;
        return At + (Interval < Left ? Interval : Left);
    }
    static uint16_t Swap16(uint16_t V) { return static_cast<uint16_t>((V << 8) | (V >> 8)); }
    static bool SameEndpoint(const FEndpoint1& A, const FEndpoint1& B)
    {
        if (A.Family != B.Family || A.Scope != B.Scope || A.PortBe != B.PortBe) return false;
        for (uint32_t I = 0; I < 16; ++I) if (A.Address[I] != B.Address[I]) return false;
        return true;
    }
    static bool EndpointValid(const FEndpoint1& E)
    {
        return E.PortBe != 0 && E.Reserved16 == 0 && ((E.Family == 4 && E.Scope == 0 && AllZero(E.Address + 4, 12)) || E.Family == 6);
    }
    static bool DecodeEndpoint(const sockaddr_storage& Storage, socklen_t N, FEndpoint1& E)
    {
        E = {};
        if (N > sizeof(Storage)) return false;
        if (Storage.ss_family == AF_INET && N >= sizeof(sockaddr_in))
        {
            const sockaddr_in& A = reinterpret_cast<const sockaddr_in&>(Storage);
            E.Family = 4; E.PortBe = A.sin_port; Copy(&A.sin_addr, E.Address, 4);
        }
        else if (Storage.ss_family == AF_INET6 && N >= sizeof(sockaddr_in6))
        {
            const sockaddr_in6& A = reinterpret_cast<const sockaddr_in6&>(Storage);
            E.Family = 6; E.PortBe = A.sin6_port; E.Scope = A.sin6_scope_id; Copy(&A.sin6_addr, E.Address, 16);
            if (AllZero(E.Address, 10) && E.Address[10] == 255 && E.Address[11] == 255)
            {
                if (E.Scope != 0) return false;
                Copy(E.Address + 12, E.Address, 4); Zero(E.Address + 4, 12); E.Family = 4;
            }
        }
        else return false;
        return EndpointValid(E);
    }
    static FEndpoint1 Normalized(FEndpoint1 E)
    {
        if (E.Family == 6 && E.Scope == 0 && AllZero(E.Address, 10) && E.Address[10] == 255 && E.Address[11] == 255)
        { Copy(E.Address + 12, E.Address, 4); Zero(E.Address + 4, 12); E.Family = 4; }
        return E;
    }
    static bool EncodeEndpoint(const FEndpoint1& E, sockaddr_storage& Storage, socklen_t& N)
    {
        Storage = {};
        if (!EndpointValid(E)) return false;
        if (E.Family == 4)
        {
            sockaddr_in& A = reinterpret_cast<sockaddr_in&>(Storage);
            A.sin_family = AF_INET; A.sin_port = E.PortBe; Copy(E.Address, &A.sin_addr, 4); N = sizeof(A);
        }
        else
        {
            sockaddr_in6& A = reinterpret_cast<sockaddr_in6&>(Storage);
            A.sin6_family = AF_INET6; A.sin6_port = E.PortBe; A.sin6_scope_id = E.Scope; Copy(E.Address, &A.sin6_addr, 16); N = sizeof(A);
        }
        return true;
    }
    static EStatus1 Creator(FNative1& S)
    {
        const pid_t P = ::getpid(); const int Error = P <= 0 ? errno : 0;
        if (P <= 0) { (void)Error; return EStatus1::NATIVE_ERROR; }
        if (static_cast<uint64_t>(P) != S.Header.CreatorProcess) return EStatus1::FORK;
        const pthread_t Self = ::pthread_self();
        if (::pthread_equal(Self, S.Header.CreatorThread) == 0) return EStatus1::WRONG_ROLE;
        Add(S, DirectCalls, 2); return EStatus1::OK;
    }
    static void InitializeFd(FFd& F, uint32_t Kind) { F = {}; F.Fd = -1; F.Kind = Kind; }
    static bool PrepareFd(FNative1& S, const FResourceBinding1* R, FFd& F, uint32_t Cell, uint32_t Location)
    {
        if (!Resources(&S, R) || Cell >= 12 || !Discharged(F)
            || R->Journal->Cells[Cell].LocalState != ResourceReserved1) return false;
        const bool Member = Cell == 1 ? (&F == &S.Listener && Location == 1 && F.Kind == ListenerKind)
            : (Cell >= 2 && Cell < 8 ? (Location >= 2 && Location < 6 && &F == &S.Pending[Location - 2].Fd && F.Kind == AcceptedKind)
            : (Cell >= 10 && Cell < 12 && Location == Cell && &F == &S.Slots[Cell - 10].Upstream && F.Kind == UpstreamKind));
        if (!Member) { ResourceRecordNotAttempted1(R, Cell); Poison(S, InvariantPoison); return false; }
        if (!Serial(S, F.Serial)) { ResourceRecordNotAttempted1(R, Cell); return false; }
        FResourceCell1& C = R->Journal->Cells[Cell];
        C.SourceSerial = F.Serial; C.SourceLocation = Location; // durable BEFORE OS
        F.Custody = ReservedFd; F.Fd = -1; F.NativeError = 0; return true;
    }
    static void ObtainedFd(FNative1& S, const FResourceBinding1* R, FFd& F, uint32_t Cell, int NativeFd)
    {
        F.Fd = NativeFd; F.Custody = Owned; F.NativeError = 0;
        ResourceRecordFdResult1(R, Cell, F.Serial, R->Journal->Cells[Cell].SourceLocation, NativeFd, 0);
        Refresh(S);
    }
    static void FailedFd(FNative1& S, const FResourceBinding1* R, FFd& F, uint32_t Cell, int Error)
    {
        F.NativeError = Error; F.Fd = -1; F.Custody = Closed;
        ResourceRecordFdResult1(R, Cell, F.Serial, R->Journal->Cells[Cell].SourceLocation, -1, Error);
        Refresh(S);
    }
    static void AbandonFd(FNative1& S, const FResourceBinding1* R, FFd& F, uint32_t Cell)
    {
        F.NativeError = 0; F.Fd = -1; F.Custody = Closed;
        ResourceRecordNotAttempted1(R, Cell); Refresh(S);
    }
    static bool CloseFd(FNative1& S, const FResourceBinding1* R, FFd& F, bool Startup = false, bool ExternalLifecycle = false)
    {
        if (!Resources(&S, R)) { Poison(S, InvariantPoison); return false; }
        if (Discharged(F)) return true;
        const uint32_t Cell = ResourceFdCell(S, R, F);
        if (F.Custody != Owned || Cell >= 12) { Poison(S, InvariantPoison); return false; }
        const FResourceCell1& C = R->Journal->Cells[Cell];
        if (C.NativeValue != static_cast<uint64_t>(F.Fd) || C.Extent
            || (C.LocalState != ResourceObtainedPending1 && C.LocalState != ResourceOwnedAck1))
        { Poison(S, InvariantPoison); return false; }
        const bool AcquisitionAcked = C.LocalState == ResourceOwnedAck1;
        if (ExternalLifecycle) OutsideCall(S, Startup); else if (!CloseCall(S, R)) return false;
        const int Fd = F.Fd; const uint64_t SerialBefore = F.Serial;
        F.Custody = Closing;
        const int Result = ::close(Fd); const int Error = Result != 0 ? errno : 0;
        // Exact terminal outcome is durable BEFORE native publication/fallible work.
        ResourceRecordFdDisposition1(R, Cell, SerialBefore, Fd, Result, Error);
        if (Result != 0)
        {
            F.NativeError = Error; F.Custody = Uncertain; Poison(S, NativePoison); Refresh(S);
            if (ExternalLifecycle && AcquisitionAcked) (void)ResourceDispatch(&S, R, nullptr, Cell, Startup);
            return false;
        }
        F.Fd = -1; F.Custody = Closed; F.NativeError = 0; Add(S, ClosedFDs); Refresh(S);
        // One initial terminal dispatch only: never retry an initial Obtained
        // inside Create/Destroy. An acquired-then-terminal row waits for outer passes.
        if (ExternalLifecycle && AcquisitionAcked) (void)ResourceDispatch(&S, R, nullptr, Cell, Startup);
        return true;
    }
    static bool Matching(const FSlot& V, const FReservation1& R)
    { return R.Size == sizeof(R) && R.Version == Version1 && R.Reserved == 0 && R.Slot < 2
        && R.Generation != 0 && R.Generation == V.Generation && R.Route == V.Route && V.Phase != Free; }
    static bool MatchingBinding(const FSlot& V, const FBinding1& B)
    { return B.Size == sizeof(B) && B.Version == Version1 && B.Reserved == 0 && B.Slot < 2
        && B.Generation != 0 && B.Generation == V.Generation && B.Route == V.Route && B.Conn != 0
        && B.Conn == V.Conn && V.Phase != Free; }
    static void Retire(FSlot& V, bool CanRetry = false)
    {
        if (V.Phase == RetryReady && !CanRetry) { V.Phase = NativeClosed; V.HeaderBytes = 0; }
        if (V.Phase != Retiring && V.Phase != NativeClosed && V.Phase != RetryReady)
        { V.Phase = Retiring; V.HeaderSent = 0; V.HeaderBytes = 0; }
        if (CanRetry) V.Flags |= RetryConnect; else V.Flags &= ~RetryConnect;
    }
    static bool Expired(FNative1& S, uint32_t I, uint64_t At)
    {
        FSlot& V = S.Slots[I];
        if (Load(S.Header.Stop) != 0 || (V.Flags & Cancelled) != 0) { Retire(V); return true; }
        if ((V.Phase != Established && At >= V.Attempt.OriginalDeadlineNs) || (V.PhaseDeadline != 0 && At >= V.PhaseDeadline)
            || (V.HalfCloseDeadline != 0 && At >= V.HalfCloseDeadline))
        { Add(S, DeadlineExpiries); Retire(V); return true; }
        return false;
    }
    static bool Fresh(FNative1& S, const FResourceBinding1* R, uint32_t I, uint64_t& At)
    {
        if (Load(S.Header.Stop) != 0) { Retire(S.Slots[I]); return false; }
        if (!Now(S, R, At)) return false;
        return !Expired(S, I, At);
    }
    static void RejectPending(FNative1& S, FPending& P, bool Protocol = false)
    {
        if (P.ParseState != PendingClosing && P.ParseState != PendingClear)
        { Add(S, Denied); if (Protocol) Add(S, ProtocolRejects); P.ParseState = PendingClosing; P.HeaderBytes = 0; }
    }

    static void AdvanceSlot(FNative1&, const FResourceBinding1*, uint32_t, short, short);
    static void AdvancePending(FNative1&, const FResourceBinding1*, uint32_t, short);
    static EStatus1 Advance(FNative1&, const FResourceBinding1*, uint32_t);
}

namespace
{
    static EStatus1 FreeMapping(FStorageJournal1& J, const FResourceBinding1* R, int32_t AbsentError = 0)
    {
        // Every validation/branch/pointer and the errno TLS slot are prepared
        // before zeroing and release. This also handles acquired Raw0 WITHOUT
        // construction/dereference. Nothing native or helper-like follows munmap.
        if (!ResourceShape(R, &J) || J.MappingBytes < sizeof(FNative1) || J.MappingBytes > 131072
            || (J.Object && !Resources(J.Object, R))) return EStatus1::RETAINED_POISON;
        FResourceCell1* const Cell = &R->Journal->Cells[0];
        if (Cell->NativeValue != reinterpret_cast<uintptr_t>(J.Raw) || Cell->Extent != J.MappingBytes
            || Cell->SourceSerial || Cell->SourceLocation
            || (Cell->LocalState != ResourceObtainedPending1 && Cell->LocalState != ResourceOwnedAck1)) return EStatus1::RETAINED_POISON;
        const uint32_t KnownState = Cell->LocalState == ResourceObtainedPending1 ? ResourceObtainedThenClosed1 : ResourceClosedPending1;
        const uint32_t UnknownState = Cell->LocalState == ResourceObtainedPending1 ? ResourceObtainedThenUncertain1 : ResourceUncertainPending1;
        uint32_t* const FaultSlot = &R->Journal->Header.Fault;
        const uint32_t UnknownFault = *FaultSlot | ResourceUncertainFault1;
        int* ErrorSlot = &errno;
        asm volatile("" : "+r"(ErrorSlot) : : "memory");
        void* const Raw = J.Raw; const size_t Bytes = static_cast<size_t>(J.MappingBytes);
        FNative1* const S = J.Object;
        if (S) Zero(S, static_cast<uint32_t>(Bytes));
        J.Object = nullptr; J.State = Releasing;
        const int Result = ::munmap(Raw, Bytes); const int Error = Result == 0 ? 0 : *ErrorSlot;
        // BOTH outcomes: ONLY prepared external scalar stores and normal return.
        Cell->Result = Result; Cell->SavedError = static_cast<uint32_t>(Error);
        if (Result == 0)
        {
            Cell->LocalState = KnownState;
            J.Raw = nullptr; J.MappingBytes = 0; J.Object = nullptr;
            J.State = JournalAbsent; J.NativeError = AbsentError; return EStatus1::OK;
        }
        Cell->LocalState = UnknownState; *FaultSlot = UnknownFault;
        J.State = JournalUncertain; J.NativeError = Error; return EStatus1::STORAGE_UNCERTAIN;
    }

    static void EncodeSecret(const uint8_t Nonce[32], char Base64[97], char Address[81])
    {
        static constexpr char Hex[] = "0123456789abcdef";
        static constexpr char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        uint8_t Raw[70] = { 'r', 'e', 'l', 'a', 'y', ':' };
        for (uint32_t I = 0; I < 32; ++I) { Raw[6 + I * 2] = Hex[Nonce[I] >> 4]; Raw[7 + I * 2] = Hex[Nonce[I] & 15]; }
        uint32_t Out = 0;
        for (uint32_t I = 0; I < 70; I += 3)
        {
            const uint32_t Left = 70 - I;
            const uint32_t V = (uint32_t(Raw[I]) << 16) | (Left > 1 ? uint32_t(Raw[I + 1]) << 8 : 0)
                | (Left > 2 ? uint32_t(Raw[I + 2]) : 0);
            Base64[Out++] = B64[(V >> 18) & 63]; Base64[Out++] = B64[(V >> 12) & 63];
            Base64[Out++] = Left > 1 ? B64[(V >> 6) & 63] : '='; Base64[Out++] = Left > 2 ? B64[V & 63] : '=';
        }
        Base64[96] = 0; Copy(Raw, Address, 70); Copy("@127.0.0.1", Address + 70, 10); Address[80] = 0; Zero(Raw, 70);
    }

    static bool SocketOption(FNative1& S, const FResourceBinding1* Binding, FFd& F, int Option, bool Startup = false)
    {
        if (Startup) OutsideCall(S, true); else if (!WorkCall(S, Binding)) return false;
        const int Requested = 16384;
        const linger Linger{0, 0};
        const void* Value = Option == SO_LINGER ? static_cast<const void*>(&Linger) : static_cast<const void*>(&Requested);
        const socklen_t N = Option == SO_LINGER ? sizeof(Linger) : sizeof(Requested);
        const int R = ::setsockopt(F.Fd, SOL_SOCKET, Option, Value, N); const int Error = R == 0 ? 0 : errno;
        if (R != 0) { F.NativeError = Error; return false; } return true;
    }

    static bool SocketQuery(FNative1& S, const FResourceBinding1* Binding, FFd& F, int Option, bool Startup = false)
    {
        if (Startup) OutsideCall(S, true); else if (!WorkCall(S, Binding)) return false;
        int Actual = 0; socklen_t N = sizeof(Actual);
        const int R = ::getsockopt(F.Fd, SOL_SOCKET, Option, &Actual, &N); const int Error = R == 0 ? 0 : errno;
        if (R != 0 || N != sizeof(Actual) || Actual <= 0 || Actual > 65536)
        { F.NativeError = R != 0 ? Error : EINVAL; return false; } return true;
    }

    static bool BufferOptionAction(FNative1& S, const FResourceBinding1* R, FFd& F, uint32_t Phase)
    {
        if (Phase == 0) return SocketOption(S, R, F, SO_LINGER);
        if (Phase == 1) return SocketOption(S, R, F, SO_SNDBUF);
        if (Phase == 2) return SocketOption(S, R, F, SO_RCVBUF);
        if (Phase == 3) return SocketQuery(S, R, F, SO_SNDBUF);
        return SocketQuery(S, R, F, SO_RCVBUF);
    }

    static bool AuthorityValid(const char* Text, uint32_t N)
    {
        if (N < 3 || N > 512) return false;
        uint32_t Port = 0;
        if (Text[0] == '[')
        {
            uint32_t Last = 1; bool Colon = false;
            for (; Last < N && Text[Last] != ']'; ++Last)
            {
                const uint8_t C = static_cast<uint8_t>(Text[Last]);
                if (C <= 32 || C > 126 || C == '[' || C == '@' || C == '/' || C == '?' || C == '#' || C == '\\') return false;
                Colon = Colon || C == ':';
            }
            if (!Colon || Last <= 1 || Last + 2 >= N || Text[Last] != ']' || Text[Last + 1] != ':') return false;
            Port = Last + 2;
        }
        else
        {
            uint32_t Colon = 0;
            for (; Colon < N && Text[Colon] != ':'; ++Colon)
            {
                const uint8_t C = static_cast<uint8_t>(Text[Colon]);
                if (C <= 32 || C > 126 || C == '[' || C == ']' || C == '@' || C == '/' || C == '?' || C == '#' || C == '\\') return false;
            }
            if (Colon == 0 || Colon + 1 >= N) return false;
            Port = Colon + 1;
        }
        uint32_t Value = 0;
        for (uint32_t I = Port; I < N; ++I)
        {
            if (Text[I] < '0' || Text[I] > '9' || Value > 6553) return false;
            Value = Value * 10 + uint32_t(Text[I] - '0'); if (Value > 65535) return false;
        }
        return Value != 0;
    }

    static bool AttemptValid(const FAttempt1& A, uint64_t Route)
    {
        if (A.Size != sizeof(A) || A.Version != Version1 || A.Route != Route || A.OriginalDeadlineNs == 0
            || A.Mode > 1 || A.CandidateCount == 0 || A.CandidateCount > 8 || A.AuthorityBytes == 0
            || A.AuthorityBytes > 512 || A.UpstreamAuthBytes > 512 || A.Reserved[0] || A.Reserved[1]
            || !AllZero(A.ReservedTail, sizeof(A.ReservedTail)) || A.Authority[A.AuthorityBytes] != 0
            || A.UpstreamAuth[A.UpstreamAuthBytes] != 0) return false;
        if (!AuthorityValid(A.Authority, A.AuthorityBytes)) return false;
        for (uint32_t I = 0; I < A.UpstreamAuthBytes; ++I)
            if (static_cast<uint8_t>(A.UpstreamAuth[I]) < 32 || static_cast<uint8_t>(A.UpstreamAuth[I]) > 126) return false;
        for (uint32_t I = 0; I < A.CandidateCount; ++I)
        {
            if (!EndpointValid(A.Candidates[I])) return false;
            for (uint32_t J = 0; J < I; ++J)
                if (SameEndpoint(Normalized(A.Candidates[I]), Normalized(A.Candidates[J]))) return false;
        }
        for (uint32_t I = A.CandidateCount; I < 8; ++I) if (!AllZero(&A.Candidates[I], sizeof(FEndpoint1))) return false;
        // Unused string tails must not smuggle retained caller data or a second request.
        return AllZero(A.Authority + A.AuthorityBytes, 513 - A.AuthorityBytes)
            && AllZero(A.UpstreamAuth + A.UpstreamAuthBytes, 513 - A.UpstreamAuthBytes);
    }

    static void Reservation(const FSlot& V, uint32_t Index, FReservation1& R)
    { R.Slot = Index; R.Generation = V.Generation; R.Route = V.Route; }

    static EStatus1 PrepareTurn(FNative1& S, const FResourceBinding1* R, bool Cleanup)
    {
        if (S.Turn.Phase != TurnClosed || Load(S.Header.OwnershipPhase) != Service) return EStatus1::BUSY;
        // Reserve32 locally first. These actual identity calls consume two of it.
        uint32_t Remaining = 32;
        --Remaining; const pid_t Process = ::getpid();
        if (Process <= 0 || uint64_t(Process) != S.Header.CreatorProcess) return EStatus1::FORK;
        --Remaining; const pthread_t Self = ::pthread_self();
        if (!::pthread_equal(Self, S.Header.ServiceThread)) return EStatus1::WRONG_ROLE;
        // Rejected role/process attempts are reported by status, without mutable ledger success.
        Add(S, DirectCalls, 2);
        if (S.Turn.Generation == UINT64_MAX)
        { Poison(S, CounterPoison); if (!Cleanup) return EStatus1::RETAINED_POISON; }
        else ++S.Turn.Generation;
        S.Turn.Phase = Cleanup ? CleanupTurn : Before; S.Turn.CallsLeft = Remaining;
        S.Turn.UsedCalls = 2; S.Turn.BytesLeft = Cleanup ? 0 : 65536; S.Turn.UsedBytes = 0;
        S.Turn.AcceptsLeft = Cleanup ? 0 : 2;
        S.Turn.FirstSlot = (S.Turn.FirstSlot + 1) % 2; S.Turn.FirstPending = (S.Turn.FirstPending + 1) % 4;
        for (uint32_t I = 0; I < 2; ++I) for (uint32_t D = 0; D < 2; ++D)
            S.Turn.DirectionBytesLeft[I][D] = Cleanup ? 0 : 16384;
        Add(S, Turns);
        if (!Cleanup)
        {
            uint64_t At = 0; if (!Now(S, R, At)) return EStatus1::RETAINED_POISON;
            S.Turn.StartedNs = At;
        }
        return Load(S.Header.Stop) ? EStatus1::STOPPING : EStatus1::OK;
    }
}

// Public bodies follow. They have ZERO external caller sites in C1.
C1_BODY EStatus1 Create1(FStorageJournal1* J, const FResourceBinding1* R)
{
    if (!ResourceShape(R, J) || !Aligned(J) || J->Size != sizeof(*J) || J->Version != Version1 || J->Reserved
        || J->State != JournalAbsent || J->Raw || J->Object || J->MappingBytes || R->Journal->Header.CreateEntered)
        return EStatus1::INVALID;
    for (const FResourceCell1& C : R->Journal->Cells)
        if (C.LocalState != ResourceEmpty1) return EStatus1::INVALID;
    R->Journal->Header.CreateEntered = 1; // Once for this graph, even initial BUSY.
    uint64_t Prefix = 0;
    ++Prefix; const pid_t Process = ::getpid(); const int ProcessError = Process <= 0 ? errno : 0;
    if (Process <= 0) { J->NativeError = ProcessError; return EStatus1::NATIVE_ERROR; }
    ++Prefix; const pthread_t Self = ::pthread_self();
    const FResourceSpec1 MapSpec = ResourceSpec(0);
    const FHookOutcome1 MapReserved = ResourceDispatch(nullptr, R, &MapSpec, 0, true, &Prefix);
    if (MapReserved.Disposition != ResourceCompleted1) return ResourceStatus(MapReserved);
    ++Prefix; const long Page = ::sysconf(_SC_PAGESIZE); const int PageError = Page <= 0 ? errno : 0;
    if (Page <= 0 || uint64_t(Page) % alignof(FNative1) != 0)
    {
        J->NativeError = PageError ? PageError : EINVAL;
        ResourceRecordNotAttempted1(R, 0); (void)ResourceDispatch(nullptr, R, nullptr, 0, true, &Prefix);
        return EStatus1::NATIVE_ERROR;
    }
    const uint64_t P = static_cast<uint64_t>(Page), Logical = sizeof(FNative1);
    const uint64_t Q = Logical / P, Tail = Logical % P;
    if (Q == UINT64_MAX || (Tail != 0 && Q + 1 > UINT64_MAX / P))
    {
        ResourceRecordNotAttempted1(R, 0); (void)ResourceDispatch(nullptr, R, nullptr, 0, true, &Prefix);
        return EStatus1::INVALID;
    }
    const uint64_t Rounded = (Q + (Tail != 0 ? 1 : 0)) * P;
    if (Rounded < Logical || Rounded > 131072 || Rounded > SIZE_MAX)
    {
        ResourceRecordNotAttempted1(R, 0); (void)ResourceDispatch(nullptr, R, nullptr, 0, true, &Prefix);
        return EStatus1::INVALID;
    }
    uint8_t Nonces[2][32]{}; char Secrets[2][97]{}, Addresses[2][81]{};
    ++Prefix; const int Rng0 = ::RAND_bytes(Nonces[0], 32);
    if (Rng0 != 1)
    {
        Zero(Nonces, sizeof(Nonces)); J->NativeError = EIO;
        ResourceRecordNotAttempted1(R, 0); (void)ResourceDispatch(nullptr, R, nullptr, 0, true, &Prefix);
        return EStatus1::NATIVE_ERROR;
    }
    ++Prefix; const int Rng1 = ::RAND_bytes(Nonces[1], 32);
    uint32_t Difference = 0; for (uint32_t I = 0; I < 32; ++I) Difference |= uint32_t(Nonces[0][I] ^ Nonces[1][I]);
    if (Rng1 != 1 || Difference == 0)
    {
        Zero(Nonces, sizeof(Nonces)); J->NativeError = EIO;
        ResourceRecordNotAttempted1(R, 0); (void)ResourceDispatch(nullptr, R, nullptr, 0, true, &Prefix);
        return EStatus1::NATIVE_ERROR;
    }
    for (uint32_t I = 0; I < 2; ++I) EncodeSecret(Nonces[I], Secrets[I], Addresses[I]);
    Zero(Nonces, sizeof(Nonces));
    ++Prefix; void* const Raw = ::mmap(nullptr, static_cast<size_t>(Rounded), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    const int MapError = Raw == MAP_FAILED ? errno : 0;
    if (Raw != MAP_FAILED)
    {
        // Actual acquisition, including successful NULL, precedes construction,
        // a second fallible operation, and even the nonallocating recording helper.
        J->Raw = Raw; J->MappingBytes = Rounded; J->Object = nullptr; J->State = Obtained; J->NativeError = 0;
    }
    else J->NativeError = MapError;
    ResourceRecordMapResult1(R, Raw, Rounded, MapError);
    (void)ResourceDispatch(nullptr, R, nullptr, 0, true, &Prefix); // one initial result attempt
    if (Raw == MAP_FAILED)
    { Zero(Secrets, sizeof(Secrets)); Zero(Addresses, sizeof(Addresses)); return EStatus1::NATIVE_ERROR; }
    if (Raw == nullptr)
    {
        Zero(Secrets, sizeof(Secrets)); Zero(Addresses, sizeof(Addresses));
        ++Prefix; const EStatus1 Released = FreeMapping(*J, R, EFAULT);
        // Full Create return is still ahead. NO receipt/helper call follows release.
        return Released == EStatus1::OK ? EStatus1::NATIVE_ERROR : Released;
    }
    FNative1* const S = ::new (Raw) FNative1{};
    S->Header.Magic = Magic1; S->Header.Version = Version1; S->Header.LogicalBytes = sizeof(FNative1);
    S->Header.MappingBytes = Rounded; S->Header.CreatorProcess = static_cast<uint64_t>(Process);
    S->Header.CreatorThread = Self; Store(S->Header.OwnershipPhase, Ready);
    S->Stats.Size = sizeof(FStats1); S->Stats.Version = Version1;
    SetCounter(*S, DirectCalls, Prefix); SetCounter(*S, StartupCalls, Prefix);
    InitializeFd(S->Listener, ListenerKind);
    for (FPending& V : S->Pending) InitializeFd(V.Fd, AcceptedKind);
    for (uint32_t I = 0; I < 2; ++I)
    {
        FSlot& V = S->Slots[I]; V.Route = I + 1;
        InitializeFd(V.Local, AcceptedKind); InitializeFd(V.Upstream, UpstreamKind);
        Copy(Secrets[I], V.ExpectedBase64, 97); Copy(Addresses[I], V.ProxyAddress, 81);
    }
    Zero(Secrets, sizeof(Secrets)); Zero(Addresses, sizeof(Addresses)); J->Object = S; J->State = Live;
    EStatus1 Failure = EStatus1::NATIVE_ERROR;
    const FResourceSpec1 ListenerSpec = ResourceSpec(1);
    if (R->Journal->Header.Fault == 0)
    {
        const FHookOutcome1 Reserved = ResourceDispatch(S, R, &ListenerSpec, 1, true);
        if (Reserved.Disposition != ResourceCompleted1) Failure = ResourceStatus(Reserved);
        else if (PrepareFd(*S, R, S->Listener, 1, 1))
        {
            OutsideCall(*S, true); const int Fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            const int Error = Fd < 0 ? errno : 0;
            if (Fd < 0) FailedFd(*S, R, S->Listener, 1, Error);
            else ObtainedFd(*S, R, S->Listener, 1, Fd);
            (void)ResourceDispatch(S, R, nullptr, 1, true); // one initial listener result
            if (Fd >= 0)
            {
                bool Good = R->Journal->Header.Fault == 0 && SocketOption(*S, R, S->Listener, SO_LINGER, true)
                    && SocketOption(*S, R, S->Listener, SO_SNDBUF, true) && SocketOption(*S, R, S->Listener, SO_RCVBUF, true)
                    && SocketQuery(*S, R, S->Listener, SO_SNDBUF, true) && SocketQuery(*S, R, S->Listener, SO_RCVBUF, true);
                sockaddr_in Address{}; Address.sin_family = AF_INET;
                uint8_t* const Ip = reinterpret_cast<uint8_t*>(&Address.sin_addr.s_addr); Ip[0] = 127; Ip[3] = 1;
                if (Good)
                {
                    OutsideCall(*S, true); const int Result = ::bind(Fd, reinterpret_cast<const sockaddr*>(&Address), sizeof(Address));
                    const int E = Result == 0 ? 0 : errno; Good = Result == 0; S->Listener.NativeError = E;
                }
                sockaddr_storage Actual{}; socklen_t N = sizeof(Actual); FEndpoint1 Endpoint{};
                if (Good)
                {
                    OutsideCall(*S, true); const int Result = ::getsockname(Fd, reinterpret_cast<sockaddr*>(&Actual), &N);
                    const int E = Result == 0 ? 0 : errno;
                    Good = Result == 0 && DecodeEndpoint(Actual, N, Endpoint) && Endpoint.Family == 4
                        && Endpoint.Address[0] == 127 && Endpoint.Address[1] == 0 && Endpoint.Address[2] == 0 && Endpoint.Address[3] == 1;
                    S->Listener.NativeError = Good ? 0 : (E != 0 ? E : EINVAL);
                }
                if (Good)
                {
                    for (FSlot& V : S->Slots) V.Tuple.Local = Endpoint;
                    OutsideCall(*S, true); const int Result = ::listen(Fd, 4); const int E = Result == 0 ? 0 : errno;
                    Good = Result == 0; S->Listener.NativeError = E;
                }
                if (Good) return R->Journal->Cells[0].LocalState == ResourceOwnedAck1
                    && R->Journal->Cells[1].LocalState == ResourceOwnedAck1 ? EStatus1::OK : EStatus1::PENDING;
                if (!CloseFd(*S, R, S->Listener, true, true)) { J->State = LivePoisoned; return EStatus1::RETAINED_POISON; }
            }
        }
        else (void)ResourceDispatch(S, R, nullptr, 1, true); // genuine no-attempt result, once
    }
    if (S->Header.Poison != 0 || R->Journal->Header.Fault) Failure = EStatus1::RETAINED_POISON;
    OutsideCall(*S, true); const EStatus1 Released = FreeMapping(*J, R);
    // Full Create has not returned until this scalar-only selection completes.
    return Released == EStatus1::OK ? Failure : Released;
}

C1_BODY EStatus1 CopyLocalRouteSecret1(FNative1* S, uint32_t Slot, FLocalRouteSecret1* Out)
{
    if (!External(S, Out) || !Output(Out) || !Object(S) || Slot >= 2) return EStatus1::INVALID;
    if (Load(S->Header.OwnershipPhase) != Ready) return EStatus1::BUSY;
    const EStatus1 Role = Creator(*S); if (Role != EStatus1::OK) return Role;
    if (Load(S->Header.Stop) || S->Header.Poison) return EStatus1::STOPPING;
    Out->Slot = Slot; Out->PortHost = Swap16(S->Slots[Slot].Tuple.Local.PortBe); Out->ProxyBytes = 80;
    Copy(S->Slots[Slot].ProxyAddress, Out->ProxyAddress, 81); return EStatus1::OK;
}

C1_BODY EStatus1 EnterService1(FNative1* S, const FResourceBinding1* R)
{
    if (!Resources(S, R)) return EStatus1::INVALID;
    if (!Object(S)) return EStatus1::INVALID;
    if (Load(S->Header.OwnershipPhase) != Ready) return EStatus1::BUSY;
    const pid_t Process = ::getpid();
    if (Process <= 0 || uint64_t(Process) != S->Header.CreatorProcess) return EStatus1::FORK;
    const pthread_t Self = ::pthread_self();
    if (::pthread_equal(Self, S->Header.CreatorThread)) return EStatus1::WRONG_ROLE;
    Add(*S, DirectCalls, 2);
    S->Header.ServiceThread = Self; Store(S->Header.OwnershipPhase, Service);
    return Load(S->Header.Stop) ? EStatus1::STOPPING : EStatus1::OK;
}

C1_BODY EStatus1 BeginTurn1(FNative1* S, const FResourceBinding1* R)
{
    if (!Resources(S, R)) return EStatus1::INVALID;
    if (R->Journal->Header.Fault) Poison(*S, NativePoison);
    return PrepareTurn(*S, R, false);
}

C1_BODY EStatus1 EndTurn1(FNative1* S, const FResourceBinding1* R)
{
    if (!Resources(S, R)) return EStatus1::INVALID;
    if (Active(S) != EStatus1::OK || S->Turn.Phase != After) return EStatus1::INVALID;
    SetCounter(*S, LastCalls, S->Turn.UsedCalls); SetCounter(*S, LastBytes, S->Turn.UsedBytes);
    S->Turn.Phase = TurnClosed; return S->Header.Poison ? EStatus1::RETAINED_POISON : EStatus1::OK;
}

// Borrowing/cost plumbing is read-only except the EXISTING turn gas/counters.
// Actual service source owns Between/code/mapping/row and one-shot-stage duties.
C1_BODY EStatus1 BorrowResourceTurn1(FNative1* S, const FResourceBinding1* R, VrLwsTurnBorrow1* Out)
{
    if (!Resources(S, R) || !External(S, Out) || !ResourceExternal(R, Out, sizeof(*Out))) return EStatus1::INVALID;
    Out->Native = nullptr; Out->Generation = 0;
    if (Active(S) != EStatus1::OK || S->Turn.Phase != Between || !S->Turn.Generation) return EStatus1::STALE;
    Out->Native = S; Out->Generation = S->Turn.Generation; return EStatus1::OK;
}
namespace
{
    static FNative1* Borrowed(const VrLwsTurnBorrow1* B, const FResourceBinding1* R)
    {
        if (!Aligned(B) || !B->Native || !B->Generation) return nullptr;
        auto* S = static_cast<FNative1*>(B->Native);
        if (!Resources(S, R) || !External(S, B) || !ResourceExternal(R, B, sizeof(*B))
            || Active(S) != EStatus1::OK || S->Turn.Phase != Between || S->Turn.Generation != B->Generation) return nullptr;
        return S;
    }
}
C1_BODY bool ResourceBorrowedExternal1(const VrLwsTurnBorrow1* B, const FResourceBinding1* R, const void* P, uint64_t Bytes)
{
    FNative1* S = Borrowed(B, R);
    return S && P && Bytes && MappingExternalSpan(S, P, Bytes);
}
C1_BODY FHookOutcome1 ResourceDispatchBorrowed1(const VrLwsTurnBorrow1* B, const FResourceBinding1* R,
    const FResourceSpec1* Spec, uint32_t Cell)
{
    FNative1* S = Borrowed(B, R);
    if (!S || Cell >= 140 || (Spec && (!External(S, Spec) || !ResourceExternal(R, Spec, sizeof(*Spec))
        || !Separate(Spec, sizeof(*Spec), B, sizeof(*B)) || Spec->Cell != Cell))) return {ResourceLocalInvalid1, 0, 0, 0};
    if (Spec && (Load(S->Header.Stop) || S->Header.Poison)) return {ResourceQuotaDeferred1, 0, 0, 0};
    return ResourceDispatch(S, R, Spec, Cell);
}
C1_BODY FHookOutcome1 ResourceDebitBorrowedNamed1(const VrLwsTurnBorrow1* B, const FResourceBinding1* R,
    uint32_t Cell, uint32_t Operation)
{
    FNative1* S = Borrowed(B, R);
    if (!S || Cell < 12 || Cell >= 140 || Operation < VR_LWS_NAMED_ACQUIRE1 || Operation > VR_LWS_NAMED_TLS_SETUP1
        || Operation == VR_LWS_NAMED_ENGINE_ROOTS1)
        return {ResourceLocalInvalid1, 0, 0, 0};
    const FResourceCell1& C = R->Journal->Cells[Cell];
    const bool ClientFd = C.Kind == 5 && C.Subkind == 3;
    const bool Known = C.LocalState == ResourceObtainedPending1 || C.LocalState == ResourceOwnedAck1;
    if (!C.SourceSerial || (Operation == VR_LWS_NAMED_ACQUIRE1 ? C.LocalState != ResourceReserved1 : !Known)
        || (Operation == VR_LWS_NAMED_RNG1 && !((C.Kind == 7 && C.Subkind == 1) || (C.Kind == 8 && C.Subkind == 1))))
        return {ResourceLocalInvalid1, 0, 0, 0};
    if (Operation == VR_LWS_NAMED_TLS_SETUP1)
    {
        if (!C.NativeSerial || !C.NativeValue || C.Kind != 8 || C.Subkind != 3)
            return {ResourceLocalInvalid1, 0, 0, 0};
    }
    else if (Operation >= VR_LWS_NAMED_CONNECT1)
    {
        const bool WakeRead = Operation == VR_LWS_NAMED_READ1 && C.Kind == 9 && C.Subkind == 1;
        const bool ContextPoll = Operation == VR_LWS_NAMED_POLL1 && C.Kind == 7 && C.Subkind == 1;
        if (!C.NativeSerial || (Operation == VR_LWS_NAMED_POLL1 ? !ContextPoll : (!ClientFd && !WakeRead))
            || (Operation != VR_LWS_NAMED_POLL1 && C.NativeValue > INT32_MAX))
            return {ResourceLocalInvalid1, 0, 0, 0};
    }
    if (Operation == VR_LWS_NAMED_DISPOSE1 && C.Kind == 8 && C.Subkind >= 7)
        return {ResourceLocalInvalid1, 0, 0, 0}; // Store child has only parent release; init/index have no proved inverse.
    const bool FdClose = Operation == VR_LWS_NAMED_DISPOSE1 && ClientFd && Known && C.NativeValue <= INT32_MAX;
    // Random/wake lifecycle is creating/joined-final only, never active close credit.
    if (Operation == VR_LWS_NAMED_DISPOSE1 && ((C.Kind == 9) || (C.Kind == 8 && C.Subkind == 1)))
        return {ResourceLocalInvalid1, 0, 0, 0};
    if (Operation == VR_LWS_NAMED_ACQUIRE1 && (C.Kind == 7 || C.Kind == 6 || C.Kind == 9
        || (C.Kind == 5 && C.Subkind == 2) || (C.Kind == 8 && (C.Subkind == 1 || C.Subkind >= 7))))
        return {ResourceLocalInvalid1, 0, 0, 0}; // Creating-only source operations never borrow active service gas.
    if (Operation != VR_LWS_NAMED_DISPOSE1 && (Load(S->Header.Stop) || S->Header.Poison || R->Journal->Header.Fault))
        return {ResourceQuotaDeferred1, 0, 0, 0};
    const uint32_t Closes = NonlistenerKnown(*S) + ResourceClientCloseCredits1(R);
    const uint32_t Extra = Operation == VR_LWS_NAMED_ACQUIRE1 && ClientFd ? 1u : 0u;
    if (!S->Turn.CallsLeft || (FdClose ? S->Turn.CallsLeft < Closes
        : S->Turn.CallsLeft - 1 < Closes + Extra)) return {ResourceQuotaDeferred1, 0, 0, 0};
    // Fixed ONE real named-attempt reservation. No external claimed cost/refund.
    --S->Turn.CallsLeft; ++S->Turn.UsedCalls;
    if (!Add(*S, DirectCalls)) return {ResourceProtocolFailure1, 0, 0, 0};
    return {ResourceCompleted1, 0, 0, 0}; // No producer entry or primitive executed here.
}

C1_BODY void RequestStop1(FNative1* S)
{
    // Sole cross-thread call: externally protected object/code; atomic stop field only.
    if (Aligned(S)) Store(S->Header.Stop, 1);
}

C1_BODY EStatus1 PrepareResolveCopySlot1(const VrLwsTurnBorrow1* B, const FResourceBinding1* R,
    uint32_t Preferred, uint32_t* Out)
{
    FNative1* S = Borrowed(B, R);
    if (!S || Preferred >= 2 || !External(S, Out) || !ResourceExternal(R, Out, sizeof(*Out))
        || !Separate(B, sizeof(*B), Out, sizeof(*Out))) return EStatus1::INVALID;
    *Out = 2;
    if (Load(S->Header.Stop) || S->Header.Poison || R->Journal->Header.Fault) return EStatus1::STOPPING;
    if (S->Turn.CallsLeft <= NonlistenerKnown(*S) + ResourceClientCloseCredits1(R)) return EStatus1::PENDING;
    for (uint32_t K = 0; K < 2; ++K) {
        const uint32_t I = (Preferred + K) % 2;
        if (S->Slots[I].Phase == Free && S->Slots[I].NextGeneration != UINT64_MAX && ResourceSlotTerminal(*S, R, I)
            && ByteRoom(*S, I, 0) >= sizeof(FAttempt1)) { *Out = I; return EStatus1::OK; }
    }
    return EStatus1::BUSY; // Local no-slot observation, no native producer call.
}
C1_BODY EStatus1 BorrowedTurnTimestamp1(const VrLwsTurnBorrow1* B, const FResourceBinding1* R, uint64_t* Out)
{
    FNative1* S = Borrowed(B, R);
    if (!S || !External(S, Out) || !ResourceExternal(R, Out, sizeof(*Out))
        || !Separate(B, sizeof(*B), Out, sizeof(*Out))) return EStatus1::INVALID;
    *Out = S->Turn.StartedNs;
    return *Out ? EStatus1::OK : EStatus1::STALE;
}
C1_BODY EStatus1 ValidateCurrentWsi1(const VrLwsTurnBorrow1* B, const FResourceBinding1* R,
    const FBinding1* Binding, uint64_t Wsi)
{
    FNative1* S = Borrowed(B, R);
    if (!S || !External(S, Binding) || !ResourceExternal(R, Binding, sizeof(*Binding))
        || Binding->Size != sizeof(*Binding) || Binding->Version != Version1 || Binding->Reserved
        || Binding->Slot >= 2 || !Wsi) return EStatus1::INVALID;
    const FSlot& V = S->Slots[Binding->Slot];
    if (V.Generation != Binding->Generation || V.Route != Binding->Route || V.Conn != Binding->Conn
        || V.WsiToken != Wsi || !(V.Flags & Claimed) || (V.Flags & WsiRetired)) return EStatus1::STALE;
    return EStatus1::OK;
}
C1_BODY FHookOutcome1 DebitCurrentSocketControl1(const VrLwsTurnBorrow1* B, const FResourceBinding1* R,
    const FBinding1* Binding, uint64_t Wsi, int32_t Fd)
{
    if (Fd < 0 || ValidateCurrentWsi1(B, R, Binding, Wsi) != EStatus1::OK)
        return {ResourceLocalInvalid1, 0, 0, 0};
    uint32_t Parent = 140, Cell = 140;
    for (uint32_t I = 12; I < 140; ++I) {
        const auto& C = R->Journal->Cells[I];
        if (C.Kind == 5 && C.Subkind == 1 && C.NativeValue == Wsi && C.NativeSerial && C.SourceSerial
            && (C.LocalState == ResourceObtainedPending1 || C.LocalState == ResourceOwnedAck1)) {
            if (Parent != 140) return {ResourceLocalInvalid1, 0, 0, 0};
            Parent = I;
        }
    }
    if (Parent == 140) return {ResourceLocalInvalid1, 0, 0, 0};
    for (uint32_t I = 12; I < 140; ++I) {
        const auto& C = R->Journal->Cells[I];
        if (C.Kind == 5 && C.Subkind == 3 && C.NativeValue == static_cast<uint64_t>(Fd)
            && C.ParentSerial == R->Journal->Cells[Parent].NativeSerial && C.NativeSerial && C.SourceSerial
            && (C.LocalState == ResourceObtainedPending1 || C.LocalState == ResourceOwnedAck1)) {
            if (Cell != 140) return {ResourceLocalInvalid1, 0, 0, 0};
            Cell = I;
        }
    }
    return Cell == 140 ? FHookOutcome1{ResourceLocalInvalid1, 0, 0, 0}
        : ResourceDebitBorrowedNamed1(B, R, Cell, VR_LWS_NAMED_SOCKET_CONTROL1);
}
C1_BODY EStatus1 ObserveRetiredClientEpisode1(const VrLwsTurnBorrow1* B, const FResourceBinding1* R,
    const FBinding1* Binding, uint64_t Wsi)
{
    // Positive source callback/FULLreturn ordering is indispensable: absence
    // of an fd row alone is NEVER a physical-retirement assertion.
    FNative1* S = Borrowed(B, R);
    if (!S || !External(S, Binding) || !ResourceExternal(R, Binding, sizeof(*Binding)) || Binding->Size != sizeof(*Binding) || Binding->Version != Version1
        || Binding->Slot >= 2 || Binding->Reserved || !Wsi) return EStatus1::INVALID;
    const FSlot& V = S->Slots[Binding->Slot];
    if (!MatchingBinding(V, *Binding) || (V.WsiToken && V.WsiToken != Wsi)) return EStatus1::STALE;
    uint64_t Parent = 0;
    for (uint32_t I = 12; I < 140; ++I) {
        const auto& C = R->Journal->Cells[I];
        if (C.Kind == 5 && C.Subkind == 1 && C.NativeValue == Wsi
            && (C.LocalState == ResourceObtainedPending1 || C.LocalState == ResourceOwnedAck1)) {
            if (Parent || !C.NativeSerial || !C.SourceSerial) return EStatus1::INVALID;
            Parent = C.NativeSerial;
        }
    }
    if (!Parent) return EStatus1::STALE;
    for (uint32_t I = 12; I < 140; ++I) {
        const auto& C = R->Journal->Cells[I];
        if (C.Kind == 5 && C.Subkind == 3 && C.ParentSerial == Parent
            && C.LocalState != ResourceClosedAck1 && C.LocalState != ResourceNotObtainedAck1)
            return EStatus1::PENDING;
    }
    return EStatus1::OK;
}
C1_BODY EStatus1 ObserveActualWebSocketEstablished1(const VrLwsTurnBorrow1* B, const FResourceBinding1* R,
    const FBinding1* Binding, uint64_t Wsi, int32_t Fd)
{
    const EStatus1 Member = ValidateCurrentWsi1(B, R, Binding, Wsi);
    if (Member != EStatus1::OK || Fd < 0) return Member == EStatus1::OK ? EStatus1::INVALID : Member;
    FNative1* S = Borrowed(B, R);
    FSlot& V = S->Slots[Binding->Slot];
    if (V.Phase != Tunnel) return EStatus1::STALE;
    uint64_t At = 0;
    if (!Fresh(*S, R, Binding->Slot, At)) return V.Phase == Retiring ? EStatus1::EXPIRED : EStatus1::PENDING;
    V.Phase = Established; V.PhaseDeadline = 0;
    return EStatus1::OK;
}

C1_BODY EStatus1 ReserveFirst1(FNative1* S, const FResourceBinding1* R, uint32_t Index, const FAttempt1* A, FReservation1* Out)
{
    if (!Resources(S, R)) return EStatus1::INVALID;
    if (!ResourceExternal(R, Out, sizeof(*Out)) || !ResourceExternal(R, A, sizeof(*A))) return EStatus1::INVALID;
    if (!External(S, Out) || !Separate(A, sizeof(*A), Out, sizeof(*Out)) || !Output(Out)) return EStatus1::INVALID;
    if (!External(S, A) || Index >= 2) return EStatus1::INVALID;
    const EStatus1 Role = Active(S); if (Role != EStatus1::OK) return Role;
    FSlot& V = S->Slots[Index];
    if (Load(S->Header.Stop) || S->Header.Poison) return EStatus1::STOPPING;
    if (V.Phase != Free || !ResourceSlotTerminal(*S, R, Index)) return EStatus1::BUSY;
    if (!AttemptValid(*A, V.Route)) return EStatus1::INVALID;
    uint64_t At = 0; if (!Now(*S, R, At)) return S->Header.Poison ? EStatus1::RETAINED_POISON : EStatus1::PENDING;
    if (Load(S->Header.Stop)) return EStatus1::STOPPING;
    if (At >= A->OriginalDeadlineNs) return EStatus1::EXPIRED;
    if (V.NextGeneration == UINT64_MAX) { Poison(*S, CounterPoison); return EStatus1::RETAINED_POISON; }
    if (!Bytes(*S, Index, 0, sizeof(FAttempt1))) return EStatus1::PENDING;
    Copy(A, &V.Attempt, sizeof(*A)); V.Generation = ++V.NextGeneration;
    V.Phase = Unbound; V.SelectedCandidate = 0; V.Conn = 0; V.Flags = 0;
    V.WsiToken = 0; V.IngressSerial = 0; V.PhaseDeadline = 0; V.HalfCloseDeadline = 0;
    Reservation(V, Index, *Out); return EStatus1::OK;
}

C1_BODY EStatus1 ReserveNext1(FNative1* S, const FResourceBinding1* R, const FReservation1* Previous, FReservation1* Out)
{
    if (!Resources(S, R)) return EStatus1::INVALID;
    if (!ResourceExternal(R, Out, sizeof(*Out)) || !ResourceExternal(R, Previous, sizeof(*Previous))) return EStatus1::INVALID;
    if (!External(S, Out) || !Separate(Previous, sizeof(*Previous), Out, sizeof(*Out)) || !Output(Out)) return EStatus1::INVALID;
    if (!External(S, Previous)) return EStatus1::INVALID;
    const FReservation1 Prior = *Previous;
    if (Prior.Slot >= 2) return EStatus1::INVALID;
    const EStatus1 Role = Active(S); if (Role != EStatus1::OK) return Role;
    FSlot& V = S->Slots[Prior.Slot];
    if (!Matching(V, Prior)) return EStatus1::STALE;
    if (Load(S->Header.Stop) || S->Header.Poison) return EStatus1::STOPPING;
    if ((V.Phase == NativeClosed || V.Phase == RetryReady) && (V.Flags & WsiRetired)
        && !(V.Flags & RetryConnect) && Discharged(V.Local) && Discharged(V.Upstream)) return EStatus1::CAPACITY;
    if (V.Phase != RetryReady || !(V.Flags & RetryConnect) || !(V.Flags & WsiRetired)
        || !Discharged(V.Local) || !Discharged(V.Upstream) || V.Rings[0].Used || V.Rings[1].Used
        || !ResourceSlotTerminal(*S, R, Prior.Slot))
        return EStatus1::BUSY;
    if (V.SelectedCandidate + 1 >= V.Attempt.CandidateCount) return EStatus1::CAPACITY;
    uint64_t At = 0; if (!Now(*S, R, At)) return S->Header.Poison ? EStatus1::RETAINED_POISON : EStatus1::PENDING;
    if (Load(S->Header.Stop)) return EStatus1::STOPPING;
    if (At >= V.Attempt.OriginalDeadlineNs) return EStatus1::EXPIRED;
    if (V.NextGeneration == UINT64_MAX) { Poison(*S, CounterPoison); return EStatus1::RETAINED_POISON; }
    ++V.SelectedCandidate; V.Generation = ++V.NextGeneration; V.Conn = 0; V.WsiToken = 0;
    V.IngressSerial = 0; V.Flags = 0; V.Phase = Unbound; V.PhaseDeadline = 0; V.HalfCloseDeadline = 0;
    V.HeaderBytes = 0; V.HeaderSent = 0; V.LocalReplyBytes = 0; V.LocalReplySent = 0; V.ProxyStatus = 0;
    Reservation(V, Prior.Slot, *Out); return EStatus1::OK;
}

C1_BODY EStatus1 Bind1(FNative1* S, const FResourceBinding1* R, const FBinding1* B)
{
    if (!Resources(S, R)) return EStatus1::INVALID;
    if (!ResourceExternal(R, B, sizeof(*B))) return EStatus1::INVALID;
    if (!External(S, B) || B->Size != sizeof(*B) || B->Version != Version1 || B->Reserved || B->Slot >= 2 || B->Conn == 0)
        return EStatus1::INVALID;
    const EStatus1 Role = Active(S); if (Role != EStatus1::OK) return Role;
    FSlot& V = S->Slots[B->Slot];
    if (B->Generation == 0 || B->Generation != V.Generation || B->Route != V.Route) return EStatus1::STALE;
    if (V.Phase != Unbound || V.Conn != 0) return EStatus1::BUSY;
    uint64_t At = 0;
    if (!Fresh(*S, R, B->Slot, At)) return S->Header.Poison ? EStatus1::RETAINED_POISON : Load(S->Header.Stop) ? EStatus1::STOPPING
        : At >= V.Attempt.OriginalDeadlineNs ? EStatus1::EXPIRED : EStatus1::PENDING;
    V.Conn = B->Conn; V.Phase = Bound; return EStatus1::OK;
}

C1_BODY EStatus1 CopyClaims1(FNative1* S, FClaim1* Claims, uint32_t Capacity, uint32_t* Count)
{
    if (!External(S, Count) || !Aligned(Claims) || Capacity != 4
        || !Separate(Claims, 4 * sizeof(*Claims), Count, sizeof(*Count))
        || !MappingExternalSpan(S, Claims, 4 * sizeof(*Claims))) return EStatus1::INVALID;
    *Count = 0;
    // The actual four-record writable extent is a private caller precondition.
    for (uint32_t I = 0; I < 4; ++I)
    { Zero(&Claims[I], sizeof(FClaim1)); Claims[I].Size = sizeof(FClaim1); Claims[I].Version = Version1; }
    const EStatus1 Role = Active(S); if (Role != EStatus1::OK) return Role;
    for (uint32_t I = 0; I < 4; ++I)
    {
        const FPending& P = S->Pending[I];
        if (P.ParseState != PendingParsed || P.MatchedSlot >= 2) continue;
        const FSlot& V = S->Slots[P.MatchedSlot];
        if (V.Phase != Bound || V.Conn == 0 || V.Generation != P.ClaimGeneration || (V.Flags & Claimed)) continue;
        FClaim1& C = Claims[(*Count)++]; C.Index = I; C.Slot = P.MatchedSlot;
        C.IngressSerial = P.Serial; C.Generation = V.Generation; C.Route = V.Route; C.Conn = V.Conn; C.AcceptedTuple = P.Tuple;
    }
    return EStatus1::OK;
}

namespace
{
    static bool SameTuple(const FTuple1& A, const FTuple1& B)
    { return SameEndpoint(A.Local, B.Local) && SameEndpoint(A.Remote, B.Remote); }
    static bool RetryablePause(int Error) { return Error == EAGAIN || Error == EWOULDBLOCK || Error == EINTR; }
    static bool CallRoom(const FNative1& S, const FResourceBinding1* R, uint32_t Count = 1, uint32_t NewFd = 0)
    { return S.Turn.CallsLeft >= Count && S.Turn.CallsLeft - Count >= NonlistenerKnown(S) + ResourceClientCloseCredits1(R) + NewFd; }
    static bool Name(FNative1& S, const FResourceBinding1* Binding, int Fd, bool Peer, FEndpoint1& E)
    {
        if (!WorkCall(S, Binding)) return false;
        sockaddr_storage A{}; socklen_t N = sizeof(A);
        const int R = Peer ? ::getpeername(Fd, reinterpret_cast<sockaddr*>(&A), &N)
            : ::getsockname(Fd, reinterpret_cast<sockaddr*>(&A), &N);
        const int Error = R == 0 ? 0 : errno;
        (void)Error; return R == 0 && DecodeEndpoint(A, N, E);
    }
}

C1_BODY EStatus1 AuthorizeCurrent1(FNative1* S, const FResourceBinding1* R, const FClaim1* C, const FBinding1* B, uint64_t ActualWsiToken, int32_t BorrowedFd)
{
    if (!Resources(S, R)) return EStatus1::INVALID;
    if (!ResourceExternal(R, C, sizeof(*C)) || !ResourceExternal(R, B, sizeof(*B))) return EStatus1::INVALID;
    if (!External(S, C) || !External(S, B) || !Separate(C, sizeof(*C), B, sizeof(*B)) || C->Size != sizeof(*C) || C->Version != Version1
        || C->Index >= 4 || C->Slot >= 2 || B->Slot != C->Slot || ActualWsiToken == 0 || BorrowedFd < 0)
        return EStatus1::INVALID;
    const EStatus1 Role = Active(S); if (Role != EStatus1::OK) return Role;
    FSlot& V = S->Slots[C->Slot]; FPending& P = S->Pending[C->Index];
    // Before ANY borrowed-fd access: actual adapter membership is an external closed premise.
    if (!MatchingBinding(V, *B) || V.Phase != Bound || V.Conn == 0 || (V.Flags & Claimed)
        || C->Generation != V.Generation || C->Route != V.Route || C->Conn != V.Conn
        || P.ParseState != PendingParsed || P.MatchedSlot != C->Slot || P.Serial != C->IngressSerial
        || P.ClaimGeneration != V.Generation || !SameTuple(C->AcceptedTuple, P.Tuple)
        || !Known(P.Fd) || !Discharged(V.Local)) return EStatus1::STALE;
    const uint32_t IngressCell = ResourceFdCell(*S, R, P.Fd);
    if (IngressCell < 2 || IngressCell >= 8 || R->Journal->Cells[IngressCell].LocalState != ResourceOwnedAck1
        || R->Journal->Cells[IngressCell].SourceSerial != P.Serial || R->Journal->Header.Fault
        || !ResourceSlotTerminal(*S, R, C->Slot)) return EStatus1::PENDING;
    // Original4 observations + worst2 parent entry + fresh post-hook clock.
    if (!CallRoom(*S, R, 7) || ByteRoom(*S, C->Slot, 0) == 0) return EStatus1::PENDING;
    uint64_t At = 0;
    if (!Fresh(*S, R, C->Slot, At)) return S->Header.Poison ? EStatus1::RETAINED_POISON : Load(S->Header.Stop) ? EStatus1::STOPPING
        : At >= V.Attempt.OriginalDeadlineNs ? EStatus1::EXPIRED : EStatus1::PENDING;
    if (At >= P.IngressDeadline) { RejectPending(*S, P); return EStatus1::EXPIRED; }
    FTuple1 Borrowed{};
    if (!Name(*S, R, BorrowedFd, false, Borrowed.Local) || !Name(*S, R, BorrowedFd, true, Borrowed.Remote)
        || !SameEndpoint(Borrowed.Local, P.Tuple.Remote) || !SameEndpoint(Borrowed.Remote, P.Tuple.Local)
        || !SameEndpoint(P.Tuple.Local, V.Tuple.Local))
    { RejectPending(*S, P); return EStatus1::AUTH_DENIED; }
    if (!WorkCall(*S, R)) return EStatus1::PENDING;
    uint8_t Extra = 0; const ssize_t N = ::recv(P.Fd.Fd, &Extra, 1, MSG_PEEK | MSG_DONTWAIT);
    const int Error = N < 0 ? errno : 0;
    if (N > 0) { Bytes(*S, C->Slot, 0, 1); Add(*S, BytesRead); }
    if (N < 0 && Error == EINTR) return EStatus1::PENDING;
    if (!(N < 0 && (Error == EAGAIN || Error == EWOULDBLOCK)))
    { RejectPending(*S, P); return EStatus1::AUTH_DENIED; }
    const FResourceSpec1 ParentSpec = ResourceSpec(8 + C->Slot);
    const FHookOutcome1 Reserved = ResourceDispatch(S, R, &ParentSpec, 8 + C->Slot);
    if (Reserved.Disposition != ResourceCompleted1) return ResourceStatus(Reserved);
    uint64_t AfterHook = 0;
    if (!Fresh(*S, R, C->Slot, AfterHook) || AfterHook >= P.IngressDeadline)
    {
        ResourceRecordNotAttempted1(R, 8 + C->Slot);
        if (AfterHook >= P.IngressDeadline) { RejectPending(*S, P); return EStatus1::EXPIRED; }
        return Load(S->Header.Stop) ? EStatus1::STOPPING : S->Header.Poison ? EStatus1::RETAINED_POISON : EStatus1::PENDING;
    }
    // Exclusive infallible stores: no call/failure/native operation inside.
    FResourceCell1& Ingress = R->Journal->Cells[IngressCell];
    FResourceCell1& Parent = R->Journal->Cells[8 + C->Slot];
    V.Local = P.Fd;
    Ingress.SourceLocation = 6 + C->Slot;
    V.Tuple = P.Tuple; V.WsiToken = ActualWsiToken; V.IngressSerial = P.Serial; V.Flags |= Claimed;
    V.Phase = NeedSocket; V.PhaseDeadline = 0;
    Parent.NativeValue = V.Generation; Parent.Extent = 0; Parent.SourceSerial = V.Generation;
    Parent.SourceLocation = 8 + C->Slot; Parent.Result = 0; Parent.SavedError = 0; Parent.LocalState = ResourceObtainedPending1;
    P.Fd.Fd = -1; P.Fd.Custody = Closed; P.ParseState = PendingClear; P.HeaderBytes = 0;
    Add(*S, Authorized); Refresh(*S); return EStatus1::OK;
}

C1_BODY EStatus1 ReleaseUnbound1(FNative1* S, const FResourceBinding1* R, const FReservation1* ReservationInput)
{
    if (!Resources(S, R)) return EStatus1::INVALID;
    if (!ResourceExternal(R, ReservationInput, sizeof(*ReservationInput))) return EStatus1::INVALID;
    if (!External(S, ReservationInput) || ReservationInput->Slot >= 2) return EStatus1::INVALID;
    const EStatus1 Role = Active(S); if (Role != EStatus1::OK) return Role;
    FSlot& V = S->Slots[ReservationInput->Slot]; if (!Matching(V, *ReservationInput)) return EStatus1::STALE;
    if (V.Phase != Unbound || V.Conn != 0) return EStatus1::BUSY;
    V.Flags |= WsiRetired; Retire(V); return EStatus1::OK;
}

C1_BODY EStatus1 RetireBound1(FNative1* S, const FResourceBinding1* R, const FBinding1* B)
{
    if (!Resources(S, R)) return EStatus1::INVALID;
    if (!ResourceExternal(R, B, sizeof(*B))) return EStatus1::INVALID;
    if (!External(S, B) || B->Slot >= 2) return EStatus1::INVALID;
    const EStatus1 Role = Active(S); if (Role != EStatus1::OK) return Role;
    FSlot& V = S->Slots[B->Slot]; if (!MatchingBinding(V, *B)) return EStatus1::STALE;
    // Actual ALL-associated-WSI retirement at the creating callsite is indispensable.
    V.Flags |= WsiRetired; Retire(V, (V.Flags & RetryConnect) != 0); return EStatus1::OK;
}

C1_BODY EStatus1 Cancel1(FNative1* S, const FResourceBinding1* R, const FReservation1* ReservationInput)
{
    if (!Resources(S, R)) return EStatus1::INVALID;
    if (!ResourceExternal(R, ReservationInput, sizeof(*ReservationInput))) return EStatus1::INVALID;
    if (!External(S, ReservationInput) || ReservationInput->Slot >= 2) return EStatus1::INVALID;
    const EStatus1 Role = Active(S); if (Role != EStatus1::OK) return Role;
    FSlot& V = S->Slots[ReservationInput->Slot]; if (!Matching(V, *ReservationInput)) return EStatus1::STALE;
    if (!(V.Flags & Cancelled)) Add(*S, Canceled);
    V.Flags |= Cancelled; Retire(V); return EStatus1::OK;
}

C1_BODY EStatus1 NativeHalfReusable1(FNative1* S, const FResourceBinding1* R, const FReservation1* ReservationInput)
{
    if (!Resources(S, R)) return EStatus1::INVALID;
    if (R->Journal->Header.Fault) return EStatus1::RETAINED_POISON;
    if (!ResourceExternal(R, ReservationInput, sizeof(*ReservationInput))) return EStatus1::INVALID;
    if (!External(S, ReservationInput) || ReservationInput->Slot >= 2) return EStatus1::INVALID;
    const EStatus1 Role = Active(S); if (Role != EStatus1::OK) return Role;
    const FSlot& V = S->Slots[ReservationInput->Slot]; if (!Matching(V, *ReservationInput)) return EStatus1::STALE;
    if (!Discharged(V.Local) || !Discharged(V.Upstream) || !ResourceSlotTerminal(*S, R, ReservationInput->Slot))
        return V.Local.Custody == Uncertain || V.Upstream.Custody == Uncertain ? EStatus1::RETAINED_POISON : EStatus1::PENDING;
    return (V.Phase == NativeClosed || V.Phase == RetryReady) && V.Rings[0].Used == 0 && V.Rings[1].Used == 0
        ? EStatus1::OK : EStatus1::PENDING;
}

namespace
{
    static void PendingClearStep(FNative1& S, const FResourceBinding1* R, uint32_t I)
    {
        FPending& P = S.Pending[I]; const uint32_t Slot = P.MatchedSlot < 2 ? P.MatchedSlot : I % 2;
        if (P.HeaderBytes > 4096) { Poison(S, InvariantPoison); return; }
        const uint32_t Left = 4096 - P.HeaderBytes;
        const uint32_t Room = ByteRoom(S, Slot, 0), N = Left < Room ? Left : Room;
        if (N != 0) { Bytes(S, Slot, 0, N); Zero(S.PendingHeader[I] + P.HeaderBytes, N); P.HeaderBytes += N; }
        if (P.HeaderBytes == 4096 && Discharged(P.Fd) && ResourcePendingCanClear(S, R, I))
        {
            P.ParseState = PendingFree; P.HeaderBytes = 0; P.IngressDeadline = 0;
            P.ClaimGeneration = 0; P.HeaderEnd = 0; P.MatchedSlot = UINT32_MAX; P.Tuple = {}; Refresh(S);
        }
    }

    static void AdvancePending(FNative1& S, const FResourceBinding1* R, uint32_t I, short Events)
    {
        FPending& P = S.Pending[I];
        if (P.ParseState == PendingFree) return;
        if (P.ParseState == PendingClosing)
        {
            if (CloseFd(S, R, P.Fd)) { P.ParseState = PendingClear; P.HeaderBytes = 0; }
            return;
        }
        if (P.ParseState == PendingClear) { PendingClearStep(S, R, I); return; }
        uint64_t At = 0;
        if (Load(S.Header.Stop)) { RejectPending(S, P); return; }
        if (!Now(S, R, At)) return;
        if (At >= P.IngressDeadline) { Add(S, DeadlineExpiries); RejectPending(S, P); return; }
        if (P.ParseState >= PendingLinger && P.ParseState <= PendingQueryReceive)
        {
            if (!CallRoom(S, R)) return;
            if (!BufferOptionAction(S, R, P.Fd, P.ParseState - PendingLinger)) { RejectPending(S, P); return; }
            ++P.ParseState; return;
        }
        if (P.ParseState == PendingLocal || P.ParseState == PendingRemote)
        {
            if (!CallRoom(S, R)) return;
            FEndpoint1& E = P.ParseState == PendingLocal ? P.Tuple.Local : P.Tuple.Remote;
            if (!Name(S, R, P.Fd.Fd, P.ParseState == PendingRemote, E)
                || (P.ParseState == PendingLocal && !SameEndpoint(E, S.Slots[0].Tuple.Local)))
            { RejectPending(S, P); return; }
            ++P.ParseState; return;
        }
        if (P.ParseState == PendingParsed)
        {
            if (P.MatchedSlot >= 2) { RejectPending(S, P); return; }
            const FSlot& V = S.Slots[P.MatchedSlot];
            if (V.Phase != Bound || V.Conn == 0 || V.Generation != P.ClaimGeneration || (V.Flags & Claimed)) RejectPending(S, P);
            return;
        }
        if (P.ParseState != PendingHeader) { Poison(S, InvariantPoison); return; }
        if (!(Events & (POLLIN | POLLHUP | POLLERR))) return;
        const uint32_t Slot = I % 2;
        if (P.HeaderBytes >= 4096) { RejectPending(S, P, true); return; }
        uint32_t N = 4096 - P.HeaderBytes; const uint32_t Room = ByteRoom(S, Slot, 0); if (N > Room) N = Room;
        if (N == 0 || !WorkCall(S, R)) return;
        const ssize_t Got = ::recv(P.Fd.Fd, S.PendingHeader[I] + P.HeaderBytes, N, MSG_DONTWAIT);
        const int Error = Got < 0 ? errno : 0;
        if (Got < 0)
        {
            if (Error == EAGAIN || Error == EWOULDBLOCK) Add(S, WouldBlock);
            else if (Error != EINTR) RejectPending(S, P);
            return;
        }
        if (Got == 0) { RejectPending(S, P); return; }
        if (uint64_t(Got) > N) { Poison(S, InvariantPoison); return; }
        Bytes(S, Slot, 0, static_cast<uint32_t>(Got)); Add(S, BytesRead, static_cast<uint64_t>(Got)); P.HeaderBytes += static_cast<uint32_t>(Got);
        FIngressExpectation1 Expected[2]{};
        for (uint32_t J = 0; J < 2; ++J)
        {
            const FSlot& V = S.Slots[J]; Expected[J].Authority = V.Attempt.Authority;
            Expected[J].AuthorityBytes = (V.Phase == Bound || V.Phase == Unbound) ? V.Attempt.AuthorityBytes : 0;
            // Inactive authorities use a real NUL at their declared zero length.
            if (Expected[J].AuthorityBytes == 0) Expected[J].Authority = "";
            Expected[J].Base64 = V.ExpectedBase64; Expected[J].Base64Bytes = 96;
        }
        FParsedHeader1 Parsed{};
        const EStatus1 Result = ParseIngress1(S.PendingHeader[I], P.HeaderBytes, Expected, &Parsed);
        if (Result == EStatus1::PENDING) return;
        if (Result != EStatus1::OK || Parsed.MatchedSlot >= 2) { RejectPending(S, P, true); return; }
        FSlot& V = S.Slots[Parsed.MatchedSlot];
        if (V.Phase != Bound || V.Conn == 0 || (V.Flags & Claimed)) { RejectPending(S, P); return; }
        P.MatchedSlot = Parsed.MatchedSlot; P.ClaimGeneration = V.Generation; P.HeaderEnd = Parsed.HeaderEnd;
        if (V.Attempt.OriginalDeadlineNs < P.IngressDeadline) P.IngressDeadline = V.Attempt.OriginalDeadlineNs;
        P.ParseState = PendingParsed;
    }

    static void AcceptOne(FNative1& S, const FResourceBinding1* R)
    {
        if (!Resources(&S, R) || Load(S.Header.Stop) || S.Header.Poison || R->Journal->Header.Fault || S.Turn.AcceptsLeft == 0) return;
        uint32_t Index = 4;
        for (uint32_t J = 0; J < 4; ++J)
        { const uint32_t I = (S.Turn.FirstPending + J) % 4; if (S.Pending[I].ParseState == PendingFree && ResourcePendingCanClear(S, R, I)) { Index = I; break; } }
        const uint32_t Cell = ResourceFreeIngress(S, R);
        if (Index == 4 || Cell == 12) { Add(S, SuppressedFull); return; }
        // Existing clock1/acquire1 + worst hook2, preserving prospective close1.
        if (!CallRoom(S, R, 4, 1) || !ResourceReserveCell(S, R, Cell)) return;
        R->Journal->Cells[Cell].SourceLocation = 2 + Index;
        uint64_t At = 0;
        if (!Now(S, R, At) || Load(S.Header.Stop) || R->Journal->Header.Fault)
        { ResourceRecordNotAttempted1(R, Cell); return; }
        FPending& P = S.Pending[Index];
        if (!PrepareFd(S, R, P.Fd, Cell, 2 + Index)) return;
        --S.Turn.AcceptsLeft;
        if (!WorkCall(S, R, 1)) { AbandonFd(S, R, P.Fd, Cell); return; }
        const int Fd = ::accept4(S.Listener.Fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        const int Error = Fd < 0 ? errno : 0;
        if (Fd < 0)
        {
            FailedFd(S, R, P.Fd, Cell, Error);
            if (Error == EAGAIN || Error == EWOULDBLOCK) Add(S, WouldBlock); return;
        }
        ObtainedFd(S, R, P.Fd, Cell, Fd); P.Serial = P.Fd.Serial; P.HeaderBytes = 0; P.HeaderEnd = 0;
        P.ClaimGeneration = 0; P.MatchedSlot = UINT32_MAX;
        P.IngressDeadline = At > UINT64_MAX - Ns250ms ? UINT64_MAX : At + Ns250ms;
        P.ParseState = PendingLinger; Add(S, Accepts); Refresh(S);
        if (Load(S.Header.Stop)) RejectPending(S, P);
    }

    static void ReplyStatus(FNative1& S, uint32_t I, uint32_t Code)
    {
        FSlot& V = S.Slots[I];
        static constexpr char Success[] = "HTTP/1.1 200 Connection Established\r\n\r\n";
        static constexpr char Refused[] = "HTTP/1.1 000 Rejected\r\n\r\n";
        const uint32_t N = Code == 200 ? sizeof(Success) - 1 : sizeof(Refused) - 1;
        if (!Bytes(S, I, 1, N)) return;
        Copy(Code == 200 ? Success : Refused, V.LocalReply, N);
        if (Code != 200)
        { V.LocalReply[9] = char('0' + Code / 100); V.LocalReply[10] = char('0' + (Code / 10) % 10); V.LocalReply[11] = char('0' + Code % 10); }
        V.LocalReplyBytes = N; V.LocalReplySent = 0; V.Phase = Code == 200 ? Reply : TerminalReply;
        if (Code != 200) V.Flags |= TerminalStatus;
    }

    static void RingPeak(FNative1& S, uint32_t I, uint32_t D)
    {
        FRing& R = S.Slots[I].Rings[D];
        if (R.Used > R.Peak) R.Peak = R.Used;
        const uint32_t Index = Peak00 + I * 2 + D;
        if (R.Peak > Counter(S, Index)) SetCounter(S, Index, R.Peak);
    }

    static bool RingValid(FNative1& S, FRing& R)
    {
        if (R.Head >= 16384 || R.Used > 16384 || R.Peak > 16384 || R.Reserved)
        { Poison(S, InvariantPoison); return false; } return true;
    }

    static void RetireStep(FNative1& S, const FResourceBinding1* R, uint32_t I)
    {
        FSlot& V = S.Slots[I];
        if (!Discharged(V.Local)) { CloseFd(S, R, V.Local); return; }
        if (!Discharged(V.Upstream)) { CloseFd(S, R, V.Upstream); return; }
        if ((V.Flags & Claimed) && !(V.Flags & PairClosedRecorded))
        { ResourceRecordPairFinalized1(R, I, V.Generation); V.Flags |= PairClosedRecorded; Add(S, PairsClosed); Refresh(S); }
        if (!ResourceSlotTerminal(S, R, I)) return;
        // HeaderSent is a physical cleanup cursor ONLY in Retiring/NativeClosed.
        constexpr uint32_t PayloadClear = 2 * 16384 + 4096 + 64;
        if (V.HeaderSent > PayloadClear) { Poison(S, InvariantPoison); return; }
        if (V.HeaderSent < PayloadClear)
        {
            uint8_t* To = nullptr; uint32_t Left = 0, D = 0;
            const uint32_t C = V.HeaderSent;
            if (C < 16384) { To = S.RingPayload[I][0] + C; Left = 16384 - C; }
            else if (C < 32768) { To = S.RingPayload[I][1] + C - 16384; Left = 32768 - C; D = 1; }
            else if (C < 36864)
            { To = S.SlotHeader[I] + C - 32768; Left = 36864 - C; D = (V.Flags & HeaderFromUpstream) ? 1 : 0; }
            else { To = reinterpret_cast<uint8_t*>(V.LocalReply) + C - 36864; Left = PayloadClear - C; D = 1; }
            const uint32_t Room = ByteRoom(S, I, D), N = Left < Room ? Left : Room;
            if (N != 0) { Bytes(S, I, D, N); Zero(To, N); V.HeaderSent += N; }
            return;
        }
        V.Rings[0].Head = 0; V.Rings[0].Used = 0; V.Rings[1].Head = 0; V.Rings[1].Used = 0;
        V.LocalReplyBytes = 0; V.LocalReplySent = 0; V.Phase = NativeClosed;
        if (!(V.Flags & WsiRetired)) return;
        if ((V.Flags & RetryConnect) && !(V.Flags & Cancelled) && !Load(S.Header.Stop))
        { V.Phase = RetryReady; return; }
        if (V.HeaderBytes > sizeof(V.Attempt)) { Poison(S, InvariantPoison); return; }
        const uint32_t Left = sizeof(V.Attempt) - V.HeaderBytes, Room = ByteRoom(S, I, 0), N = Left < Room ? Left : Room;
        if (N != 0) { Bytes(S, I, 0, N); Zero(reinterpret_cast<uint8_t*>(&V.Attempt) + V.HeaderBytes, N); V.HeaderBytes += N; }
        if (V.HeaderBytes != sizeof(V.Attempt)) return;
        // Keep permanent route/capability, generation history and lifetime ring peaks.
        V.Conn = 0; V.WsiToken = 0; V.IngressSerial = 0; V.SelectedCandidate = 0;
        V.Tuple.Remote = {}; V.ActualPeer = {}; V.Flags = 0; V.PhaseDeadline = 0; V.HalfCloseDeadline = 0;
        V.HeaderBytes = 0; V.HeaderSent = 0; V.ProxyStatus = 0; V.Phase = Free;
    }

    static void TunnelDirection(FNative1& S, const FResourceBinding1* Binding, uint32_t I, uint32_t D, short ReadEvents, short WriteEvents)
    {
        FSlot& V = S.Slots[I]; FRing& R = V.Rings[D]; if (!RingValid(S, R)) return;
        const FFd& Input = D == 0 ? V.Local : V.Upstream; const FFd& OutputFd = D == 0 ? V.Upstream : V.Local;
        const uint32_t Eof = D == 0 ? EofLocal : EofUpstream, Shut = D == 0 ? ShutUpstream : ShutLocal;
        uint64_t At = 0;
        if (R.Used != 0 && (WriteEvents & (POLLOUT | POLLERR | POLLHUP)))
        {
            uint32_t N = 16384 - R.Head; if (N > R.Used) N = R.Used;
            const uint32_t Room = ByteRoom(S, I, D); if (N > Room) N = Room;
            if (N == 0 || !Fresh(S, Binding, I, At) || !WorkCall(S, Binding)) return;
            const ssize_t Sent = ::send(OutputFd.Fd, S.RingPayload[I][D] + R.Head, N, MSG_NOSIGNAL | MSG_DONTWAIT);
            const int Error = Sent < 0 ? errno : 0;
            if (Sent < 0)
            { if (Error == EAGAIN || Error == EWOULDBLOCK) Add(S, WouldBlock); else if (Error != EINTR) Retire(V); return; }
            if (Sent > 0)
            {
                if (uint64_t(Sent) > N) { Poison(S, InvariantPoison); return; }
                const uint32_t Count = static_cast<uint32_t>(Sent); Bytes(S, I, D, Count); Add(S, BytesWritten, Count);
                R.Head = (R.Head + Count) % 16384; R.Used -= Count;
            }
            return; // At most one bounded I/O action per direction in this visit.
        }
        if ((V.Flags & Eof) && R.Used == 0 && !(V.Flags & Shut))
        {
            if (!Fresh(S, Binding, I, At) || !WorkCall(S, Binding)) return;
            const int Result = ::shutdown(OutputFd.Fd, SHUT_WR); const int Error = Result == 0 ? 0 : errno;
            if (Result != 0) { (void)Error; Retire(V); return; } V.Flags |= Shut; return;
        }
        if ((V.Flags & Eof) || !(ReadEvents & (POLLIN | POLLERR | POLLHUP))) return;
        if (R.Used == 16384) { Add(S, SuppressedFull); return; }
        const uint32_t Tail = (R.Head + R.Used) % 16384;
        uint32_t N = 16384 - Tail; if (N > 16384 - R.Used) N = 16384 - R.Used;
        const uint32_t Room = ByteRoom(S, I, D); if (N > Room) N = Room;
        if (N == 0 || !Fresh(S, Binding, I, At) || !WorkCall(S, Binding)) return;
        const ssize_t Got = ::recv(Input.Fd, S.RingPayload[I][D] + Tail, N, MSG_DONTWAIT);
        const int Error = Got < 0 ? errno : 0;
        if (Got < 0)
        { if (Error == EAGAIN || Error == EWOULDBLOCK) Add(S, WouldBlock); else if (Error != EINTR) Retire(V); return; }
        if (Got == 0)
        {
            V.Flags |= Eof;
            if (V.Phase == Established && At > UINT64_MAX - Ns250ms) { Retire(V); return; }
            const uint64_t End = V.Phase == Established ? At + Ns250ms
                : Deadline(At, Ns250ms, V.Attempt.OriginalDeadlineNs);
            if (V.HalfCloseDeadline == 0 || End < V.HalfCloseDeadline) V.HalfCloseDeadline = End;
            return;
        }
        if (uint64_t(Got) > N) { Poison(S, InvariantPoison); return; }
        const uint32_t Count = static_cast<uint32_t>(Got); Bytes(S, I, D, Count); Add(S, BytesRead, Count);
        R.Used += Count; RingPeak(S, I, D);
    }

    static void AdvanceSlot(FNative1& S, const FResourceBinding1* R, uint32_t I, short LocalEvents, short UpstreamEvents)
    {
        FSlot& V = S.Slots[I];
        if (V.Phase == Free) return;
        if (V.Phase == Retiring || V.Phase == NativeClosed) { RetireStep(S, R, I); return; }
        if (V.Phase == RetryReady)
        {
            uint64_t At = 0;
            if (Load(S.Header.Stop) || (V.Flags & Cancelled)) Retire(V);
            else if (Now(S, R, At) && At >= V.Attempt.OriginalDeadlineNs) { Add(S, DeadlineExpiries); Retire(V); }
            return;
        }
        if (V.Phase == NeedSocket)
        {
            if (!Resources(&S, R) || Load(S.Header.Stop) || S.Header.Poison || R->Journal->Header.Fault)
            { Retire(V); return; }
            const FResourceCell1& Parent = R->Journal->Cells[8 + I];
            if (!(V.Flags & Claimed) || !Known(V.Local) || V.SelectedCandidate >= V.Attempt.CandidateCount
                || Parent.SourceSerial != V.Generation || Parent.NativeValue != V.Generation
                || (Parent.LocalState != ResourceObtainedPending1 && Parent.LocalState != ResourceOwnedAck1))
            { Poison(S, InvariantPoison); Retire(V); return; }
            if (!CallRoom(S, R, 4, 1) || !ResourceReserveCell(S, R, 10 + I, Parent.NativeSerial)) return;
            uint64_t AfterHook = 0;
            if (!Fresh(S, R, I, AfterHook)) { ResourceRecordNotAttempted1(R, 10 + I); return; }
            if (!PrepareFd(S, R, V.Upstream, 10 + I, 10 + I)) { Retire(V); return; }
            if (!WorkCall(S, R, 1)) { AbandonFd(S, R, V.Upstream, 10 + I); Retire(V); return; }
            const int Family = V.Attempt.Candidates[V.SelectedCandidate].Family == 4 ? AF_INET : AF_INET6;
            const int Fd = ::socket(Family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); const int Error = Fd < 0 ? errno : 0;
            if (Fd < 0) { FailedFd(S, R, V.Upstream, 10 + I, Error); Retire(V); return; }
            ObtainedFd(S, R, V.Upstream, 10 + I, Fd); Add(S, UpstreamCreated); V.Phase = SetupLinger;
            if (Load(S.Header.Stop)) Retire(V); return;
        }
        uint64_t At = 0; if (!Fresh(S, R, I, At)) return;
        if (V.Phase == Unbound || V.Phase == Bound) return;
        if (V.Phase >= SetupLinger && V.Phase <= QueryReceive)
        {
            if (!CallRoom(S, R)) return;
            if (!BufferOptionAction(S, R, V.Upstream, V.Phase - SetupLinger)) { Retire(V); return; }
            ++V.Phase; return;
        }
        if (V.Phase == Connect)
        {
            // One connect for this socket. A raced Stop still leaves actual custody charged.
            if (V.Flags & ConnectIssued) { Poison(S, InvariantPoison); Retire(V); return; }
            sockaddr_storage Address{}; socklen_t N = 0;
            if (!EncodeEndpoint(V.Attempt.Candidates[V.SelectedCandidate], Address, N)) { Retire(V); return; }
            if (!WorkCall(S, R)) return;
            V.Flags |= ConnectIssued;
            if (V.Attempt.Mode == 1) V.PhaseDeadline = Deadline(At, Ns2s, V.Attempt.OriginalDeadlineNs);
            const int Result = ::connect(V.Upstream.Fd, reinterpret_cast<const sockaddr*>(&Address), N);
            const int Error = Result == 0 ? 0 : errno;
            if (Result == 0) V.Phase = CheckPeer;
            else if (Error == EINPROGRESS || Error == EALREADY || Error == EWOULDBLOCK || Error == EINTR) V.Phase = Connecting;
            else { V.Upstream.NativeError = Error; Retire(V, true); }
            return;
        }
        if (V.Phase == Connecting)
        {
            if (!(UpstreamEvents & (POLLOUT | POLLERR | POLLHUP)) || !WorkCall(S, R)) return;
            int ErrorValue = 0; socklen_t N = sizeof(ErrorValue);
            const int Result = ::getsockopt(V.Upstream.Fd, SOL_SOCKET, SO_ERROR, &ErrorValue, &N);
            const int Error = Result == 0 ? 0 : errno;
            if (Result != 0 || N != sizeof(ErrorValue)) { V.Upstream.NativeError = Result != 0 ? Error : EINVAL; Retire(V); }
            else if (ErrorValue != 0) { V.Upstream.NativeError = ErrorValue; Retire(V, true); }
            else V.Phase = CheckPeer;
            return;
        }
        if (V.Phase == CheckPeer)
        {
            if (!CallRoom(S, R)) return;
            FEndpoint1 Actual{};
            if (!Name(S, R, V.Upstream.Fd, true, Actual)
                || !SameEndpoint(Actual, Normalized(V.Attempt.Candidates[V.SelectedCandidate])))
            { Retire(V); return; }
            V.ActualPeer = Actual; V.Flags |= ConnectedPeer;
            // One proxy-header phase includes request construction/write and response read.
            V.PhaseDeadline = V.Attempt.Mode == 1 ? Deadline(At, Ns2s, V.Attempt.OriginalDeadlineNs) : 0;
            V.Phase = ProxyBuild; return;
        }
        if (V.Phase == ProxyBuild)
        {
            if (V.Attempt.Mode == 0) { ReplyStatus(S, I, 200); return; }
            constexpr char Start[] = "CONNECT ", VersionHost[] = " HTTP/1.1\r\nHost: ";
            constexpr char Auth[] = "\r\nProxy-Authorization: ";
            const uint32_t A = V.Attempt.AuthorityBytes, U = V.Attempt.UpstreamAuthBytes;
            const uint32_t N = (sizeof(Start) - 1) + A + (sizeof(VersionHost) - 1) + A
                + (U != 0 ? (sizeof(Auth) - 1) + U : 0) + 4;
            if (N > 4096) { Retire(V); return; }
            if (!Bytes(S, I, 0, N)) return;
            uint32_t C = 0; uint8_t* const To = S.SlotHeader[I];
            Copy(Start, To + C, sizeof(Start) - 1); C += sizeof(Start) - 1;
            Copy(V.Attempt.Authority, To + C, A); C += A;
            Copy(VersionHost, To + C, sizeof(VersionHost) - 1); C += sizeof(VersionHost) - 1;
            Copy(V.Attempt.Authority, To + C, A); C += A;
            if (U != 0) { Copy(Auth, To + C, sizeof(Auth) - 1); C += sizeof(Auth) - 1; Copy(V.Attempt.UpstreamAuth, To + C, U); C += U; }
            Copy("\r\n\r\n", To + C, 4); V.HeaderBytes = N; V.HeaderSent = 0; V.Phase = ProxySend; return;
        }
        if (V.Phase == ProxySend)
        {
            if (!(UpstreamEvents & (POLLOUT | POLLERR | POLLHUP))) return;
            if (V.HeaderSent > V.HeaderBytes || V.HeaderBytes > 4096) { Poison(S, InvariantPoison); Retire(V); return; }
            uint32_t N = V.HeaderBytes - V.HeaderSent; const uint32_t Room = ByteRoom(S, I, 0); if (N > Room) N = Room;
            if (N == 0 || !WorkCall(S, R)) return;
            const ssize_t Sent = ::send(V.Upstream.Fd, S.SlotHeader[I] + V.HeaderSent, N, MSG_NOSIGNAL | MSG_DONTWAIT);
            const int Error = Sent < 0 ? errno : 0;
            if (Sent < 0) { if (Error == EAGAIN || Error == EWOULDBLOCK) Add(S, WouldBlock); else if (Error != EINTR) Retire(V); return; }
            if (uint64_t(Sent) > N) { Poison(S, InvariantPoison); Retire(V); return; }
            Bytes(S, I, 0, static_cast<uint32_t>(Sent)); Add(S, BytesWritten, static_cast<uint64_t>(Sent)); V.HeaderSent += static_cast<uint32_t>(Sent);
            if (V.HeaderSent == V.HeaderBytes)
            {
                V.Phase = ProxyRequestClear; V.HeaderSent = 0;
                // Preserve the verified-peer phase deadline; completing send cannot renew it.
            }
            return;
        }
        if (V.Phase == ProxyRequestClear || V.Phase == ProxyResponseClear)
        {
            const uint32_t D = V.Phase == ProxyRequestClear ? 0 : 1;
            if (V.HeaderSent > 4096) { Poison(S, InvariantPoison); Retire(V); return; }
            const uint32_t Left = 4096 - V.HeaderSent, Room = ByteRoom(S, I, D), N = Left < Room ? Left : Room;
            if (N != 0) { Bytes(S, I, D, N); Zero(S.SlotHeader[I] + V.HeaderSent, N); V.HeaderSent += N; }
            if (V.HeaderSent != 4096) return;
            if (V.Phase == ProxyRequestClear)
            {
                V.HeaderBytes = 0; V.HeaderSent = 0; V.Flags |= HeaderFromUpstream;
                V.Phase = ProxyReceive; // Preserve the deadline established at completed request send.
            }
            else ReplyStatus(S, I, 200);
            return;
        }
        if (V.Phase == ProxyReceive)
        {
            if (!(UpstreamEvents & (POLLIN | POLLHUP | POLLERR))) return;
            if (V.HeaderBytes >= 4096) { Retire(V); Add(S, ProtocolRejects); return; }
            uint32_t N = 4096 - V.HeaderBytes; const uint32_t Room = ByteRoom(S, I, 1); if (N > Room) N = Room;
            if (N == 0 || !WorkCall(S, R)) return;
            const ssize_t Got = ::recv(V.Upstream.Fd, S.SlotHeader[I] + V.HeaderBytes, N, MSG_DONTWAIT);
            const int Error = Got < 0 ? errno : 0;
            if (Got < 0) { if (Error == EAGAIN || Error == EWOULDBLOCK) Add(S, WouldBlock); else if (Error != EINTR) Retire(V); return; }
            if (Got == 0) { Retire(V); return; }
            if (uint64_t(Got) > N) { Poison(S, InvariantPoison); Retire(V); return; }
            Bytes(S, I, 1, static_cast<uint32_t>(Got)); Add(S, BytesRead, static_cast<uint64_t>(Got)); V.HeaderBytes += static_cast<uint32_t>(Got);
            FParsedHeader1 Parsed{}; const EStatus1 ParsedStatus = ParseProxyResponse1(S.SlotHeader[I], V.HeaderBytes, &Parsed);
            if (ParsedStatus == EStatus1::PENDING) return;
            if (ParsedStatus != EStatus1::OK) { Add(S, ProtocolRejects); Retire(V); return; }
            V.ProxyStatus = Parsed.StatusCode; V.PhaseDeadline = 0; // Complete final header ends this phase.
            if (Parsed.StatusCode != 200)
            {
                // Preserve the actual code. Headers/suffix remain charged until terminal retirement.
                V.Phase = TerminalReply; V.LocalReplyBytes = 0; ReplyStatus(S, I, V.ProxyStatus); return;
            }
            V.HeaderSent = Parsed.HeaderEnd; V.Phase = ProxySuffix; return;
        }
        if (V.Phase == ProxySuffix)
        {
            if (V.HeaderSent > V.HeaderBytes || V.HeaderBytes > 4096) { Poison(S, InvariantPoison); Retire(V); return; }
            FRing& SuffixRing = V.Rings[1]; if (!RingValid(S, SuffixRing)) { Retire(V); return; }
            const uint32_t Tail = (SuffixRing.Head + SuffixRing.Used) % 16384;
            uint32_t N = V.HeaderBytes - V.HeaderSent;
            if (N > 16384 - SuffixRing.Used) N = 16384 - SuffixRing.Used;
            if (N > 16384 - Tail) N = 16384 - Tail;
            const uint32_t Room = ByteRoom(S, I, 1); if (N > Room) N = Room;
            if (N != 0)
            { Bytes(S, I, 1, N); Copy(S.SlotHeader[I] + V.HeaderSent, S.RingPayload[I][1] + Tail, N); V.HeaderSent += N; SuffixRing.Used += N; RingPeak(S, I, 1); }
            if (V.HeaderSent == V.HeaderBytes) { V.HeaderSent = 0; V.Phase = ProxyResponseClear; V.PhaseDeadline = 0; }
            return;
        }
        if (V.Phase == Reply || V.Phase == TerminalReply)
        {
            if (V.LocalReplyBytes == 0) { ReplyStatus(S, I, V.ProxyStatus); return; }
            if (!(LocalEvents & (POLLOUT | POLLERR | POLLHUP))) return;
            if (V.LocalReplySent > V.LocalReplyBytes || V.LocalReplyBytes > sizeof(V.LocalReply)) { Poison(S, InvariantPoison); Retire(V); return; }
            uint32_t N = V.LocalReplyBytes - V.LocalReplySent; const uint32_t Room = ByteRoom(S, I, 1); if (N > Room) N = Room;
            if (N == 0 || !WorkCall(S, R)) return;
            const ssize_t Sent = ::send(V.Local.Fd, V.LocalReply + V.LocalReplySent, N, MSG_NOSIGNAL | MSG_DONTWAIT);
            const int Error = Sent < 0 ? errno : 0;
            if (Sent < 0) { if (Error == EAGAIN || Error == EWOULDBLOCK) Add(S, WouldBlock); else if (Error != EINTR) Retire(V); return; }
            if (uint64_t(Sent) > N) { Poison(S, InvariantPoison); Retire(V); return; }
            Bytes(S, I, 1, static_cast<uint32_t>(Sent)); Add(S, BytesWritten, static_cast<uint64_t>(Sent)); V.LocalReplySent += static_cast<uint32_t>(Sent);
            if (V.LocalReplySent == V.LocalReplyBytes)
            { if (V.Phase == TerminalReply) Retire(V); else { V.Phase = Tunnel; V.PhaseDeadline = 0; } }
            return;
        }
        if (V.Phase == Tunnel || V.Phase == Established)
        {
            const uint32_t First = S.Turn.ProgressCursor;
            if (First >= 2) { Poison(S, InvariantPoison); Retire(V); return; }
            S.Turn.ProgressCursor = (First + 1) % 2;
            for (uint32_t K = 0; K < 2 && (V.Phase == Tunnel || V.Phase == Established); ++K)
            {
                const uint32_t D = (First + K) % 2;
                TunnelDirection(S, R, I, D, D == 0 ? LocalEvents : UpstreamEvents, D == 0 ? UpstreamEvents : LocalEvents);
            }
            if ((V.Phase == Tunnel || V.Phase == Established) && (V.Flags & (EofLocal | EofUpstream)) == (EofLocal | EofUpstream)
                && V.Rings[0].Used == 0 && V.Rings[1].Used == 0 && (V.Flags & (ShutLocal | ShutUpstream)) == (ShutLocal | ShutUpstream)) Retire(V);
            return;
        }
        Poison(S, InvariantPoison); Retire(V);
    }

    static EStatus1 Advance(FNative1& S, const FResourceBinding1* R, uint32_t ExpectedPhase)
    {
        if (S.Turn.Phase != ExpectedPhase) return EStatus1::INVALID;
        pollfd Fds[9]{};
        for (pollfd& F : Fds) F.fd = -1;
        if (Known(S.Listener) && !Load(S.Header.Stop) && !S.Header.Poison) { Fds[0].fd = S.Listener.Fd; Fds[0].events = POLLIN; }
        for (uint32_t I = 0; I < 4; ++I)
        {
            const FPending& P = S.Pending[I];
            if (Known(P.Fd) && P.ParseState != PendingClosing && P.ParseState != PendingClear)
            { Fds[1 + I].fd = P.Fd.Fd; Fds[1 + I].events = POLLIN; }
        }
        for (uint32_t I = 0; I < 2; ++I)
        {
            const FSlot& V = S.Slots[I];
            if (Known(V.Local))
            {
                pollfd& F = Fds[5 + I * 2]; F.fd = V.Local.Fd;
                if (V.Phase == Reply || V.Phase == TerminalReply) F.events = POLLOUT;
                else if (V.Phase == Tunnel || V.Phase == Established)
                { if (!(V.Flags & EofLocal) && V.Rings[0].Used < 16384) F.events |= POLLIN; if (V.Rings[1].Used) F.events |= POLLOUT; }
            }
            if (Known(V.Upstream))
            {
                pollfd& F = Fds[6 + I * 2]; F.fd = V.Upstream.Fd;
                if (V.Phase == Connecting || V.Phase == ProxySend) F.events = POLLOUT;
                else if (V.Phase == ProxyReceive) F.events = POLLIN;
                else if (V.Phase == Tunnel || V.Phase == Established)
                { if (!(V.Flags & EofUpstream) && V.Rings[1].Used < 16384) F.events |= POLLIN; if (V.Rings[0].Used) F.events |= POLLOUT; }
            }
        }
        if (!Load(S.Header.Stop) && WorkCall(S, R))
        {
            const int Result = ::poll(Fds, 9, 0); const int Error = Result < 0 ? errno : 0;
            if (Result < 0 && Error != EINTR) Poison(S, NativePoison);
            if (Result < 0) for (pollfd& F : Fds) F.revents = 0;
        }
        for (const pollfd& F : Fds) if (F.revents & POLLNVAL) Poison(S, NativePoison);
        for (uint32_t K = 0; K < 2; ++K)
        {
            const uint32_t I = (S.Turn.FirstSlot + K) % 2;
            AdvanceSlot(S, R, I, Fds[5 + I * 2].revents, Fds[6 + I * 2].revents);
        }
        for (uint32_t K = 0; K < 4; ++K)
        { const uint32_t I = (S.Turn.FirstPending + K) % 4; AdvancePending(S, R, I, Fds[1 + I].revents); }
        if (Fds[0].revents & POLLIN) AcceptOne(S, R);
        S.Turn.Phase = ExpectedPhase == Before ? Between : After;
        Refresh(S); return S.Header.Poison ? EStatus1::RETAINED_POISON : Load(S.Header.Stop) ? EStatus1::STOPPING : EStatus1::OK;
    }
}

C1_BODY EStatus1 AdvanceBefore1(FNative1* S, const FResourceBinding1* R)
{
    if (!Resources(S, R)) return EStatus1::INVALID;
    const EStatus1 Role = Active(S);
    if (Role != EStatus1::OK || S->Turn.Phase != Before) return Role == EStatus1::OK ? EStatus1::INVALID : Role;
    ResourceReconcile(*S, R); return Advance(*S, R, Before);
}
C1_BODY EStatus1 AdvanceAfter1(FNative1* S, const FResourceBinding1* R)
{
    if (!Resources(S, R)) return EStatus1::INVALID;
    const EStatus1 Role = Active(S);
    if (Role != EStatus1::OK) return Role;
    if (R->Journal->Header.Fault) Poison(*S, NativePoison);
    const EStatus1 Result = Advance(*S, R, Between);
    if (S->Turn.Phase == After) ResourceReconcile(*S, R);
    return Result;
}

namespace
{
    template<bool PublishReceipts> static EStatus1 FinalPairs(FNative1& S, const FResourceBinding1* R, uint32_t FinalPhase)
    {
        Store(S.Header.Stop, 1);
        bool Clean = true;
        for (FPending& P : S.Pending)
        {
            if (P.ParseState != PendingFree) P.ParseState = PendingClosing;
            if (!CloseFd(S, R, P.Fd)) Clean = false;
        }
        for (uint32_t I = 0; I < 2; ++I)
        {
            FSlot& V = S.Slots[I];
            if (V.Phase != Free) Retire(V);
            if (!CloseFd(S, R, V.Local)) Clean = false;
            if (!CloseFd(S, R, V.Upstream)) Clean = false;
            if ((V.Flags & Claimed) && Discharged(V.Local) && Discharged(V.Upstream) && !(V.Flags & PairClosedRecorded))
            { ResourceRecordPairFinalized1(R, I, V.Generation); V.Flags |= PairClosedRecorded; Add(S, PairsClosed); }
        }
        if constexpr (PublishReceipts) ResourceReconcile(S, R);
        Refresh(S); SetCounter(S, LastCalls, S.Turn.UsedCalls); SetCounter(S, LastBytes, S.Turn.UsedBytes);
        // Finalized is a known pair-close observation only. It is NEVER actual join proof.
        if (Clean && Counter(S, UncertainFDs) == 0 && NonlistenerKnown(S) == 0)
        { S.Turn.Phase = TurnClosed; Store(S.Header.OwnershipPhase, FinalPhase); return EStatus1::OK; }
        // Retain the one final turn/remaining credits; a repeated call cannot mint another.
        S.Turn.Phase = CleanupTurn;
        return EStatus1::RETAINED_POISON;
    }
}

C1_BODY EStatus1 StopPairs1(FNative1* S, const FResourceBinding1* R)
{
    if (!Resources(S, R)) return EStatus1::INVALID;
    if (!Object(S)) return EStatus1::INVALID;
    const uint32_t Phase = Load(S->Header.OwnershipPhase);
    // Only the same protected service invocation may make this idempotent observation.
    if (Phase == Finalized) return EStatus1::OK;
    if (Phase != Service) return EStatus1::WRONG_ROLE;
    if (!TurnActive(*S))
    {
        const EStatus1 Prepared = PrepareTurn(*S, R, true);
        if (Prepared != EStatus1::OK && Prepared != EStatus1::STOPPING) return Prepared;
    }
    // An active turn keeps EXACTLY its remaining close credits. No replenishment.
    return FinalPairs<true>(*S, R, Finalized);
}

C1_BODY EStatus1 StopUnregisteredServicePairs1(FNative1* S, const FResourceBinding1* R)
{
    if (!Resources(S, R)) return EStatus1::INVALID;
    const uint32_t Phase = Load(S->Header.OwnershipPhase);
    if (Phase == Finalized) return EStatus1::OK;
    if (Phase != Service) return EStatus1::WRONG_ROLE;
    if (!TurnActive(*S))
    {
        const EStatus1 Prepared = PrepareTurn(*S, R, true);
        if (Prepared != EStatus1::OK && Prepared != EStatus1::STOPPING) return Prepared;
    }
    // Actual unregistered Run/early-Stop ONLY. No native role appointment,
    // resource entry, registration retry, GT service call or no-child inference.
    return FinalPairs<false>(*S, R, Finalized);
}

C1_BODY EStatus1 StopNeverStartedPairs1(FNative1* S, const FResourceBinding1* R)
{
    if (!Resources(S, R)) return EStatus1::INVALID;
    if (!Object(S)) return EStatus1::INVALID;
    const uint32_t Phase = Load(S->Header.OwnershipPhase);
    if (Phase != Ready && Phase != NeverStartedFinalized) return EStatus1::WRONG_ROLE;
    // GENUINE no-created-thread outcome at the creating site is a closed prerequisite.
    // Ready/absence of Enter is merely consistency; it cannot establish that fact.
    const EStatus1 Role = Creator(*S); if (Role != EStatus1::OK) return Role;
    if (Phase == NeverStartedFinalized) return EStatus1::OK;
    if (S->Turn.Phase != TurnClosed) return EStatus1::BUSY;
    if (S->Turn.Generation == UINT64_MAX) Poison(*S, CounterPoison);
    else ++S->Turn.Generation;
    S->Turn.Phase = CleanupTurn; S->Turn.CallsLeft = 30; S->Turn.UsedCalls = 2;
    S->Turn.BytesLeft = 0; S->Turn.UsedBytes = 0; S->Turn.AcceptsLeft = 0; Add(*S, Turns);
    return FinalPairs<true>(*S, R, NeverStartedFinalized);
}

C1_BODY EStatus1 CopyStats1(const FNative1* S, FStats1* Out)
{
    if (!External(S, Out) || !Output(Out) || !Object(S)) return EStatus1::INVALID;
    const uint32_t Phase = Load(S->Header.OwnershipPhase);
    if (Phase == Service)
    {
        // Same verified exclusive invocation, never a concurrent GT poll.
        if (!TurnActive(*S)) return EStatus1::WRONG_ROLE;
    }
    else if (Phase == Finalized || Phase == NeverStartedFinalized)
    {
        // Actual native join/no-child and immutable post-service lifetime are prerequisites.
        const EStatus1 Role = Creator(*const_cast<FNative1*>(S)); if (Role != EStatus1::OK) return Role;
    }
    else return EStatus1::WRONG_ROLE;
    Out->Phase = Phase; Out->Poison = S->Header.Poison;
    for (uint32_t I = 0; I < CounterCount1; ++I) Out->Counters[I] = Counter(*S, I);
    return EStatus1::OK;
}

C1_BODY EStatus1 DestroyAfterContext1(FStorageJournal1* J, const FResourceBinding1* R)
{
    if (!ResourceShape(R, J)) return EStatus1::INVALID;
    if (!Aligned(J) || J->Size != sizeof(*J) || J->Version != Version1 || J->Reserved != 0) return EStatus1::INVALID;
    if (J->State == JournalUncertain || J->State == LivePoisoned) return EStatus1::RETAINED_POISON;
    if (J->State != Live || J->Raw == nullptr || J->Object != J->Raw || !Resources(J->Object, R)
        || J->MappingBytes != J->Object->Header.MappingBytes) return EStatus1::INVALID;
    if (!Separate(J, sizeof(*J), J->Raw, J->MappingBytes)) return EStatus1::INVALID;
    FNative1& S = *J->Object;
    const uint32_t Phase = Load(S.Header.OwnershipPhase);
    if (Phase != Finalized && Phase != NeverStartedFinalized)
    {
        if (Phase == Service && Counter(S, UncertainFDs) != 0)
        {
            const EStatus1 Role = Creator(S); if (Role != EStatus1::OK) return Role;
            J->State = LivePoisoned; return EStatus1::RETAINED_POISON;
        }
        return EStatus1::BUSY;
    }
    const EStatus1 Role = Creator(S); if (Role != EStatus1::OK) return Role;
    // Actual join, context/all WSI/vhost destruction and hook release are caller prerequisites.
    // No ContextClosed/Joined boolean can grant them.
    for (const FPending& P : S.Pending) if (!Discharged(P.Fd)) return EStatus1::RETAINED_POISON;
    for (const FSlot& V : S.Slots)
        if (!Discharged(V.Local) || !Discharged(V.Upstream)
            || ((V.Flags & Claimed) && !(V.Flags & PairClosedRecorded))) return EStatus1::RETAINED_POISON;
    // LAST actual socket close. An ambiguous result retains the live mapping, with no retry.
    if (!CloseFd(S, R, S.Listener, false, true)) { J->State = LivePoisoned; return EStatus1::RETAINED_POISON; }
    if (Counter(S, KnownFDs) != 0 || Counter(S, UncertainFDs) != 0)
    { J->State = LivePoisoned; return EStatus1::RETAINED_POISON; }
    OutsideCall(S); return FreeMapping(*J, R);
}
}
#undef C1_BODY
#endif
