// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "VaRestNumericConnectorResource1.h"

namespace VaRest::WebSocket::NumericConnectorC1
{
enum class EStatus1 : int32_t
{
    OK = 0, PENDING = 1, BUSY = 2, CAPACITY = 3, INVALID = 4, STALE = 5,
    STOPPING = 6, EXPIRED = 7, WRONG_ROLE = 8, FORK = 9, NATIVE_ERROR = 10,
    PROTOCOL = 11, AUTH_DENIED = 12, RETAINED_POISON = 13, STORAGE_UNCERTAIN = 14
};
enum : uint32_t { Slots1 = 2, Pending1 = 4, RingBytes1 = 16384, HeaderBytes1 = 4096,
    Candidates1 = 8, Version1 = 1, CounterCount1 = 30 };
struct FNative1;
struct FEndpoint1
{
    uint32_t Family, Scope;
    uint16_t PortBe, Reserved16;
    uint8_t Address[16];
};
struct FTuple1 { FEndpoint1 Local, Remote; };
struct FAttempt1
{
    uint32_t Size, Version;
    uint64_t Route, OriginalDeadlineNs;
    uint32_t Mode, CandidateCount;
    FEndpoint1 Candidates[8];
    uint32_t AuthorityBytes, UpstreamAuthBytes, Reserved[2];
    char Authority[513], UpstreamAuth[513];
    uint8_t ReservedTail[6];
};
struct FReservation1
{
    uint32_t Size, Version, Slot, Reserved;
    uint64_t Generation, Route;
};
struct FBinding1
{
    uint32_t Size, Version, Slot, Reserved;
    uint64_t Generation, Route, Conn;
};
struct FClaim1
{
    uint32_t Size, Version, Index, Slot;
    uint64_t IngressSerial, Generation, Route, Conn;
    FTuple1 AcceptedTuple;
};
struct FLocalRouteSecret1
{
    uint32_t Size, Version, Slot, Reserved;
    uint16_t PortHost, ProxyBytes;
    uint32_t Reserved2;
    char ProxyAddress[81];
    uint8_t ReservedTail[7];
};
struct FStats1 { uint32_t Size, Version, Phase, Poison; uint64_t Counters[30]; };
struct FStorageJournal1
{
    uint32_t Size, Version;
    void* Raw;
    uint64_t MappingBytes;
    FNative1* Object;
    uint32_t State;
    int32_t NativeError;
    uint64_t Reserved;
};
struct FParsedHeader1 { uint32_t Outcome, HeaderEnd, MatchedSlot, StatusCode; };
struct FIngressExpectation1
{
    const char* Authority;
    const char* Base64;
    uint32_t AuthorityBytes, Base64Bytes;
};

// Private source surface, not an exported API, Supplier, pin or admission packet.
// Caller already protects actual mapping/code, one legitimate lease and role.
// Linux x86_64 is the sole implemented source target. Dormant L4 has actual
// private source callers; no default/Module/Host selection is authorized.
// Creator is the actual initialized physical GT by the later caller's proof;
// pthread identity/phase/journal/tuples cannot establish that authority or lifetime.
// Enter/Begin establish one actual service invocation, protected through End/Stop.
// RequestStop alone may race it. No fork or replacement thread during that interval.
// Read-only output spans must also be disjoint from the creating resource holder.
// CopyClaims needs four complete nonaliasing writable records and a distinct count.
// ReserveNext/RetireBound require genuine complete associated-WSI retirement;
// AuthorizeCurrent additionally requires real current Conn/generation/WSI membership
// BEFORE the caller obtains the borrowed LWS fd. The core never closes that fd.
// Destroy requires actual no-child/join then context/WSI/vhost destruction/hooks off;
// mapping, creating WS code and dependency protection continue through final return.
EStatus1 Create1(FStorageJournal1*, const FResourceBinding1* R);
EStatus1 CopyLocalRouteSecret1(FNative1*, uint32_t, FLocalRouteSecret1*);
EStatus1 EnterService1(FNative1*, const FResourceBinding1* R);
EStatus1 BeginTurn1(FNative1*, const FResourceBinding1* R);
EStatus1 AdvanceBefore1(FNative1*, const FResourceBinding1* R);
EStatus1 AdvanceAfter1(FNative1*, const FResourceBinding1* R);
EStatus1 EndTurn1(FNative1*, const FResourceBinding1* R);
// Read-only generation borrow, valid ONLY during this actual Between interval.
// No identity/clock/OS import and no second active quota; spans remain external.
EStatus1 BorrowResourceTurn1(FNative1*, const FResourceBinding1*, VrLwsTurnBorrow1*);
FHookOutcome1 ResourceDispatchBorrowed1(const VrLwsTurnBorrow1*, const FResourceBinding1*,
    const FResourceSpec1*, uint32_t cell);
bool ResourceBorrowedExternal1(const VrLwsTurnBorrow1*, const FResourceBinding1*, const void*, uint64_t bytes);
FHookOutcome1 ResourceDebitBorrowedNamed1(const VrLwsTurnBorrow1*, const FResourceBinding1*,
    uint32_t cell, uint32_t operation);
// Pure current-slot/gas preflight BEFORE consumptive ABI2 copy_result. Same
// exclusive source interval immediately ReserveFirst's returned index; no reserve.
EStatus1 PrepareResolveCopySlot1(const VrLwsTurnBorrow1*, const FResourceBinding1*, uint32_t preferred, uint32_t* slot);
// Pure existing captured monotonic time; only a current actual Between borrow.
EStatus1 BorrowedTurnTimestamp1(const VrLwsTurnBorrow1*, const FResourceBinding1*, uint64_t*);
// Source already proved current Conn/WSI membership before obtaining the borrowed fd.
EStatus1 ValidateCurrentWsi1(const VrLwsTurnBorrow1*, const FResourceBinding1*, const FBinding1*, uint64_t wsi);
FHookOutcome1 DebitCurrentSocketControl1(const VrLwsTurnBorrow1*, const FResourceBinding1*, const FBinding1*, uint64_t wsi, int32_t fd);
// Callsite already observed old WSI_DESTROY + whole tracked service return;
// Step4 FD ACK/physical retirement precedes that callback in the selected C body.
EStatus1 ObserveRetiredClientEpisode1(const VrLwsTurnBorrow1*, const FResourceBinding1*, const FBinding1*, uint64_t wsi);
EStatus1 ObserveActualWebSocketEstablished1(const VrLwsTurnBorrow1*, const FResourceBinding1*, const FBinding1*, uint64_t wsi, int32_t fd);
EStatus1 ReserveFirst1(FNative1*, const FResourceBinding1* R, uint32_t, const FAttempt1*, FReservation1*);
EStatus1 ReserveNext1(FNative1*, const FResourceBinding1* R, const FReservation1*, FReservation1*);
EStatus1 Bind1(FNative1*, const FResourceBinding1* R, const FBinding1*);
EStatus1 CopyClaims1(FNative1*, FClaim1*, uint32_t, uint32_t*);
EStatus1 AuthorizeCurrent1(FNative1*, const FResourceBinding1* R, const FClaim1*, const FBinding1*, uint64_t, int32_t);
EStatus1 ReleaseUnbound1(FNative1*, const FResourceBinding1* R, const FReservation1*);
EStatus1 RetireBound1(FNative1*, const FResourceBinding1* R, const FBinding1*);
EStatus1 Cancel1(FNative1*, const FResourceBinding1* R, const FReservation1*);
EStatus1 NativeHalfReusable1(FNative1*, const FResourceBinding1* R, const FReservation1*);
void RequestStop1(FNative1*);
EStatus1 StopPairs1(FNative1*, const FResourceBinding1* R);
EStatus1 StopNeverStartedPairs1(FNative1*, const FResourceBinding1* R);
// Actual unregistered Run only: fixed journal-only closes, never native dispatch.
EStatus1 StopUnregisteredServicePairs1(FNative1*, const FResourceBinding1* R);
EStatus1 CopyStats1(const FNative1*, FStats1*);
EStatus1 DestroyAfterContext1(FStorageJournal1*, const FResourceBinding1* R);
EStatus1 ParseIngress1(const uint8_t*, uint32_t, const FIngressExpectation1[2], FParsedHeader1*);
EStatus1 ParseProxyResponse1(const uint8_t*, uint32_t, FParsedHeader1*);
}
