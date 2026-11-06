// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeOwnerCallerEntry.h"
#include "VaRestNativeOwnerPump.h"
#include "../VaRestLwsService.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <errno.h>
#include <type_traits>

namespace VaRest::WebSocket::Lws
{
namespace CallerEntryDispositionDetail1
{
    enum : uint32 { Fresh=0, GateHeld=1, SupplierReturned=2,
        EnteringNative=3, NativeReturned=4, Finished=5, Retained=6, NativeReturnedBit=0x100 };
    static_assert(sizeof(void*)==8 && sizeof(FCallerBootstrapFn1)==8);
    static_assert(std::is_standard_layout_v<FCallerReadFd1>
        &&std::is_trivial_v<FCallerReadFd1>&&std::is_trivially_copyable_v<FCallerReadLedger1>);
    static_assert(std::is_trivially_copyable_v<FCallerPrivateBinding1>
        &&std::is_trivially_destructible_v<FCallerEJournal1>);
    static bool BindingShape1(const FCallerPrivateBinding1& B)
    {
        return B.InventorySchema==2&&B.PresentMask==0x3fff&&B.Admit&&B.Getter
            &&B.NeverCreated&&B.RegisterCurrentThread&&B.JoinObserved&&B.RequestStop
            &&B.PumpRegistered&&B.PumpEnter&&B.PumpLeave&&B.RequestDetachAfterJoin
            &&B.Reserve&&B.Obtained&&B.NotObtained&&B.Closed&&B.Uncertain;
    }
    static void ClearSelectedCallables1(FCallerPrivateBinding1& B)
    {
        B.NeverCreated=nullptr;B.RegisterCurrentThread=nullptr;B.JoinObserved=nullptr;
        B.RequestStop=nullptr;B.PumpRegistered=nullptr;B.PumpEnter=nullptr;
        B.PumpLeave=nullptr;B.RequestDetachAfterJoin=nullptr;B.Admit=nullptr;B.Getter=nullptr;
        B.Reserve=nullptr;B.Obtained=nullptr;B.NotObtained=nullptr;B.Closed=nullptr;B.Uncertain=nullptr;
        // Only historical nonoperational Bias/schema/mask remains. No selected
        // O is inferred from NativeOutcome.image, and raw output is untouched.
    }
    static bool NativeFrame1(const VrBootstrap1& B,VrStatus ActualStatus)
    {
        if(B.size!=112||B.version!=1)return false;
        if(ActualStatus!=VR_STATUS_OK)
        {
            // c14 refusal framing is exactly zero112 plus size/version, not
            // the coherent success frame. Ordinary known refusals can discharge
            // Core1 after their E/fds close; actual19 still dominates in Run.
            const uint8_t* Bytes=reinterpret_cast<const uint8_t*>(&B);
            for(uint32_t I=8;I<112;++I)if(Bytes[I]!=0)return false;
            return true;
        }
        if(B.magic!=VR_BOOTSTRAP_MAGIC||B.pointer_bits!=64
            ||B.endian!=VR_ENDIAN_LITTLE||B.owner_abi!=2
            ||B.flags&~uint32_t(VR_BOOTSTRAP_FLAGS_MASK)||B.reserved[0]||B.reserved[1])return false;
        // The producer may reuse a compatible FOREIGN normal owner. Do not
        // require B.image.hi==SelectedEntry.Bias or install selected private
        // addresses for that owner. Only the genuine F0/Q4 successful call graph
        // and continuously protected original A object permit the fixed handoff.
        return B.flags==VR_BOUND_CONTROL&&B.image.hi!=0&&B.image.lo!=0
            &&B.owner&&reinterpret_cast<uintptr_t>(B.owner)%8==0
            &&B.api&&reinterpret_cast<uintptr_t>(B.api)%8==0;
    }
    static bool FilesDischarged1(const FCallerReadLedger1& L)
    {
        for(const FCallerReadFd1& F:L.Cells)
            if(F.Custody!=CallerFdEmpty1&&F.Custody!=CallerFdClosed1)return false;
        return true;
    }
    static bool EntryDischarged1(const FCallerEJournal1& E)
    { return E.State==CallerEEmpty1||E.State==CallerEClosed1; }
}

[[gnu::used]] FCallerEntryTransaction::FCallerEntryTransaction()
    :Files{{{-1,0,0,0,0},{-1,0,0,0,0}},0},Entry{},SelectedEntry{},NativeOutcome{},
      Gate{},Inspections(0),Phase(CallerEntryDispositionDetail1::Fresh),
      NativeStatus(VR_STATUS_NO_RESULT),DispositionStatus(VR_STATUS_NO_RESULT)
{
    // Exactly one real object/frame. No second binding/bootstrap snapshot, heap,
    // resource installer, reset method or automatic cleanup route.
    NativeOutcome.size=112;NativeOutcome.version=1;NativeOutcome.magic=VR_BOOTSTRAP_MAGIC;
    NativeOutcome.pointer_bits=64;NativeOutcome.endian=VR_ENDIAN_LITTLE;NativeOutcome.owner_abi=2;
}

[[gnu::used]] VrStatus FCallerEntryTransaction::RunForService(FService& Service)
{
    using namespace CallerEntryDispositionDetail1;
    if(Phase!=Fresh)return VR_STATUS_STALE;
    Phase=Retained; // Once even a refused prefix cannot reuse this local object.
    VrStatus Status=RequireNativeOwnerControlThread(TEXT("source original caller entry"));
    if(Status!=VR_STATUS_OK){DispositionStatus=Status;return Status;}
    if(!NativeOwnerRealThreadModeAllowed())
    {DispositionStatus=VR_STATUS_FORK_UNESTABLISHED;return DispositionStatus;}
    // Actual H/immutable-before-first-load/preload/history/full Core+WS/provider
    // return lifetime are closed source prerequisites, never an argument/bit.
    Status=Gate.Acquire(Inspections);
    if(Status!=VR_STATUS_OK){DispositionStatus=Status;return Status;}
    Phase=GateHeld;
    Status=FNativeSourceSupplier::QualifyOriginalEntryWithScratch1(*this);
    // The ordinary OUT-OF-LINE scratch-owning function has FULLY returned before
    // any native call. Fresh actual frame/lowering proof MUST establish this;
    // no noinline/global flag, inline160/1280 borrow or source-only waiver.
    Phase=SupplierReturned;
    bool Permanent=Status==VR_STATUS_RELEASE_UNCERTAIN;
    if(Status==VR_STATUS_OK)
    {
        if(Entry.State!=CallerEVerified1||Entry.Handle==nullptr||!BindingShape1(SelectedEntry))
        {Status=VR_STATUS_PROVENANCE;Permanent=true;}
        else
        {
            Phase=EnteringNative;
            NativeStatus=SelectedEntry.Admit(sizeof(NativeOutcome),&NativeOutcome);
            // Record the REAL native status before E/file disposition or any
            // other fallible action; no status normalizer can erase actual19.
            // Preserve this actual return fact through final disposition, also
            // when raw native NO_RESULT17 equals the never-called diagnostic.
            Phase=NativeReturned|NativeReturnedBit;
            asm volatile("" : "+m"(NativeStatus) : : "memory");
            Status=NativeStatus;
            if(NativeStatus<VR_STATUS_OK||NativeStatus>VR_STATUS_RELEASE_UNCERTAIN)
            {Status=VR_STATUS_PROVENANCE;Permanent=true;}
            if(!NativeFrame1(NativeOutcome,NativeStatus))
            {if(NativeStatus!=VR_STATUS_RELEASE_UNCERTAIN)Status=VR_STATUS_PROVENANCE;Permanent=true;}
            if(NativeStatus==VR_STATUS_RELEASE_UNCERTAIN)Permanent=true;
        }
    }
    // Full native return (or genuine no-call qualification failure) is past.
    // Never expose selected callable bits after its E can be final/unmapped,
    // including foreign normal-owner reuse. No provisional callable escapes.
    ClearSelectedCallables1(SelectedEntry);
    // Direct source edge, never an installer/callback or selected-B packet.
    // FULL qualified Q4 success supplies current A; H excludes every direct or
    // reentrant retire until its true lease commits. No extra lookup/query.
    VrStatus HandoffStatus=VR_STATUS_NO_RESULT;
    if(Status==VR_STATUS_OK && NativeStatus==VR_STATUS_OK && (Phase&NativeReturnedBit))
    {
        HandoffStatus=Service.AttachActualOwnerDuringEntry(*this);
        if(HandoffStatus!=VR_STATUS_OK)Status=HandoffStatus;
        if(HandoffStatus==VR_STATUS_RELEASE_UNCERTAIN)Permanent=true;
    }
    // Only fully VERIFIED E receives the sole certified external close. This
    // comes AFTER FULL native unwind/return (or a known no-call path). A cold E
    // can be final: actual H must cover finalizers/list/Core+WS/dependency and
    // this close's complete return; no nonfinal-S theorem is borrowed here.
    if(Entry.State==CallerEVerified1)
    {
        Entry.State=CallerEClosing1;
        asm volatile("" : : : "memory");
        const int Result=::dlclose(Entry.Handle);
        const int Error=Result!=0?errno:0;
        Entry.CloseResult=Result;Entry.SavedError=Error;
        Entry.State=Result==0?CallerEClosed1:CallerEUncertain1;
        // Nonzero has unknown credit disposition. errno is a raw diagnostic,
        // not dlclose's success contract. Never dlerror/query/close/retry it.
        if(Result!=0){Permanent=true;Status=VR_STATUS_RELEASE_UNCERTAIN;}
    }
    else if(!EntryDischarged1(Entry))
    {
        // In particular unverified unexpected E is RETAINED, never guessed
        // closed merely because its path/map/count/query looked plausible.
        Permanent=true;
    }
    const VrStatus FilesClosed=FNativeSourceSupplier::CloseKnownCallerReadFds1(Files);
    if(FilesClosed!=VR_STATUS_OK||!FilesDischarged1(Files))
    {Permanent=true;Status=VR_STATUS_RELEASE_UNCERTAIN;}
    if(NativeStatus==VR_STATUS_RELEASE_UNCERTAIN)
    {Permanent=true;Status=VR_STATUS_RELEASE_UNCERTAIN;}
    if(!EntryDischarged1(Entry))Permanent=true;
    // Record actual no-create duty before THIS SAME owned gate is finished.
    // E/fd failure after attach never starts a resource or child. Retained
    // BUSY/journals remain in the real strong service graph, never pointer guesses.
    const VrStatus HandoffFinished=Service.FinishNativeOwnerEntryDisposition(*this,Status);
    if(HandoffFinished==VR_STATUS_RELEASE_UNCERTAIN || HandoffStatus==VR_STATUS_RELEASE_UNCERTAIN)
    {Permanent=true;Status=VR_STATUS_RELEASE_UNCERTAIN;}
    else if(Status==VR_STATUS_OK && HandoffFinished!=VR_STATUS_OK)Status=HandoffFinished;
    // No opens follow reader/native/close uncertainty. All independent known
    // fds received their single permitted close, with original kept through E.
    if(Permanent)
    {
        Phase=Retained|(Phase&NativeReturnedBit);
        const VrStatus GateStatus=Gate.MakePermanent(Inspections);
        // A failed exact2 publication leaves the already-owned nonzero gate.
        // Every actual19 dominates even if all subsequent known closes succeed.
        if(Status!=VR_STATUS_RELEASE_UNCERTAIN&&GateStatus!=VR_STATUS_OK)Status=GateStatus;
        if(Status==VR_STATUS_OK)Status=VR_STATUS_PROVENANCE;
    }
    else
    {
        const VrStatus GateStatus=Gate.CompleteKnownDischarge(NativeStatus,Inspections);
        if(GateStatus!=VR_STATUS_OK){Phase=Retained|(Phase&NativeReturnedBit);Status=GateStatus;}
        else Phase=Finished|(Phase&NativeReturnedBit);
    }
    DispositionStatus=Status;
    // NativeOutcome/NativeStatus remain untouched actual records. The separate
    // service journal and charged A lease record the genuine handoff. No
    // selected-B callable, owner==selected test or operational packet escapes.
    return Status;
}
}
#endif
