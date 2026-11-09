// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)

#include "VaRestNativeOwnerAbi.h"
#include "VaRestNativeOwnerPrivateService1.h"
#include "VaRestNativeOwnerPrivateResource1.h"
#include "VaRestNativeOwnerPrivateApi1.h"
namespace VaRest::WebSocket::Lws
{
    class FTransport;
    class FService;
    class FCallerEntryTransaction;
    // Actual transaction custody/data only. These records grant no source, host,
    // mapping, role, immutable-deployment or callback/return lifetime authority.
    struct FCallerReadFd1
    {
        int32_t Fd; uint32_t Custody; int32_t NativeError; uint32_t Kind;
        uint64_t AcquisitionOrdinal;
    };
    struct FCallerReadLedger1 { FCallerReadFd1 Cells[2]; uint64_t NextOrdinal; };
    enum : uint32_t { CallerFdEmpty1=0, CallerFdReserved1=1, CallerFdKnown1=2,
        CallerFdClosing1=3, CallerFdClosed1=4, CallerFdUncertain1=5 };
    struct FCallerEJournal1
    {
        void* Handle; uint64_t AcquisitionOrdinal;
        uint32_t State; int32_t CloseResult, SavedError; uint32_t Reserved;
        void* OpaqueMap; int64_t Namespace;
    };
    enum : uint32_t { CallerEEmpty1=0, CallerEReserved1=1, CallerEUnverified1=2,
        CallerEVerified1=3, CallerEClosing1=4, CallerEClosed1=5, CallerEUncertain1=6 };
    using FCallerBootstrapFn1=VrStatus (VR_CALL *)(uint32_t, VrBootstrap1*);
    // Reader3 selects full14/storage216 with genuinely qualified full-source Profile2.
    // Original32 consistency uses the same original fd/Io4096/attempts before E;
    // it authenticates nothing and reads NO mapped tail or foreign owner here.
    // Selected entry-image addresses, NOT a binding to any returned foreign
    // owner. Ordinary complete receipt types/closures and H remain prerequisites.
    struct FCallerPrivateBinding1
    {
        VrPrivateSessionStatus1Fn NeverCreated, RegisterCurrentThread, JoinObserved;
        VrPrivateSessionVoid1Fn RequestStop;
        VrPrivatePumpRegistered1Fn PumpRegistered;
        VrPrivateSessionStatus1Fn PumpEnter;
        VrPrivateSessionVoid1Fn PumpLeave, RequestDetachAfterJoin;
        VrPrivateAdmitControl1Fn Admit;
        FCallerBootstrapFn1 Getter;
        uint64_t Bias;
        uint32_t InventorySchema, PresentMask;
        VrPrivateResourceReserve1Fn Reserve;
        VrPrivateResourceObtained1Fn Obtained;
        VrPrivateResourceNotObtained1Fn NotObtained;
        VrPrivateResourceClosed1Fn Closed;
        VrPrivateResourceUncertain1Fn Uncertain;
    };
    // ZERO production callers. The sole future FTransport source lane must own
    // the actual transaction/graph and all closed H/GT/fork/custody prerequisites.
    // No path, host/ready flag, installer, callback or externally supplied receipt.
    class [[gnu::visibility("hidden")]] FNativeSourceSupplier final
    {
        friend class FTransport;
        friend class FService;
        friend class FCallerEntryTransaction;
        FNativeSourceSupplier()=delete;
        static VrStatus TryCreateForCurrentControl(FCallerEntryTransaction&, FService&);
        // Owns the SOLE explicit256256 scratch in its separate ordinary frame.
        // MUST return/pop before real Admit. Actual frame evidence is mandatory.
        static VrStatus QualifyOriginalEntryWithScratch1(FCallerEntryTransaction&);
        static VrStatus CloseKnownCallerReadFds1(FCallerReadLedger1&);
    };
    static_assert(sizeof(FCallerReadFd1)==24 && alignof(FCallerReadFd1)==8);
    static_assert(offsetof(FCallerReadFd1, Fd)==0 && offsetof(FCallerReadFd1, Custody)==4);
    static_assert(offsetof(FCallerReadFd1, NativeError)==8 && offsetof(FCallerReadFd1, Kind)==12);
    static_assert(offsetof(FCallerReadFd1, AcquisitionOrdinal)==16);
    static_assert(sizeof(FCallerReadLedger1)==56 && sizeof(FCallerEJournal1)==48);
    static_assert(offsetof(FCallerEJournal1, State)==16 && offsetof(FCallerEJournal1, CloseResult)==20);
    static_assert(offsetof(FCallerEJournal1, SavedError)==24 && offsetof(FCallerEJournal1, Reserved)==28);
    static_assert(offsetof(FCallerEJournal1, OpaqueMap)==32 && offsetof(FCallerEJournal1, Namespace)==40);
    static_assert(sizeof(FCallerPrivateBinding1)==136 && alignof(FCallerPrivateBinding1)==8);
    static_assert(offsetof(FCallerPrivateBinding1, Admit)==64);
    static_assert(offsetof(FCallerPrivateBinding1, Getter)==72);
    static_assert(offsetof(FCallerPrivateBinding1, Bias)==80);
    static_assert(offsetof(FCallerPrivateBinding1, InventorySchema)==88);
    static_assert(offsetof(FCallerPrivateBinding1, PresentMask)==92);
    static_assert(offsetof(FCallerPrivateBinding1, Reserve)==96);
}
#endif
