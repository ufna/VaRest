// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeOwnerAdmission.h"
#include "VaRestNativeOwnerDiscovery.h"
#include "VaRestNativeOwnerState.h"
#include "VaRestNativeOwnerServiceBridge.h"
#include "VaRestNativeOwnerProducer.h"
#include "VaRestNativeOwnerPrivateApi1.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>

namespace VaRestNativeOwnerPrivate
{
namespace
{
    using namespace Detail;
    using namespace Deployment1;
    using namespace Discovery1;

    // Scalar transaction locals, not another API/object copy or pin ledger.
    // Scratch256256 + phase<=1024 + owner overhead<=512 remain the exact
    // preexisting partitions. Every emitted helper/spill/return frame counts
    // against4608; source assertions do not measure the actual final frames.
    struct FAdmissionLocal4
    {
        FQDiscovery1 Discovery;
        FQReadLedger1 Files;
        VrSnapshot Snapshot;
        VrRetire Request;
        VrRetired Transfer;
        Dl_info Info;
        void* AnchorMap;
        struct link_map* HandleMap;
        const char* Path;
        Lmid_t Namespace;
        uint32_t ResumePhase, Reserved;
    };
    static_assert(sizeof(FAdmissionLocal4)<=512);
    static_assert(std::is_trivial_v<FAdmissionLocal4> && std::is_trivially_destructible_v<FAdmissionLocal4>);

    static uint32_t Read32(const uint8_t* P)
    { return uint32_t(P[0])|uint32_t(P[1])<<8|uint32_t(P[2])<<16|uint32_t(P[3])<<24; }
    static const uint8_t* CatalogueRecord(const FQScratchStorage1& S,uint32_t I)
    { return S.Catalogue+VR_Q_HEADER_BYTES+uint64_t(I)*VR_Q_RECORD_BYTES; }
    static bool CurrentPrivateRecord(const uint8_t* R)
    {
        if(Read32(R)!=VR_Q_CLASS_LINEAGE||Read32(R+172)!=VR_Q_API_PRIVATE_OBJECT_BYTES)return false;
        for(uint32_t I=0;I<14;++I)if(Read32(R+176+8*I)==0||Read32(R+180+8*I)==0)return false;
        return true; // Raw storage/inventory shape only, never source authority.
    }
    enum class EQualifiedNormalProfile2 : uint32_t { None, Legacy72, Historical1, Current2 };
    static VrStatus ClassifyQualifiedNormalProfile2(const FQScratchStorage1& S,const FAdmissionLocal4& C,
        EQualifiedNormalProfile2& Profile)
    {
        Profile=EQualifiedNormalProfile2::None;
        const uint32_t I=C.Discovery.NormalOwnerRecord;
        if(I==UINT32_MAX)return VR_STATUS_OK;
        const uint8_t* R=CatalogueRecord(S,I);
        const FQParsedLayout1& L=S.Candidates[I];const VrBootstrap1& B=L.Bootstrap;
        const uint64_t Rva=Read32(R+168),Storage=Read32(R+172);
        if(!B.owner||!B.api||!(B.flags&VR_BOUND_CONTROL)||(B.flags&(VR_POISONED|VR_RETIRED)))return VR_STATUS_ABI_INCOMPATIBLE;
        if(Rva>UINTPTR_MAX-L.Bias||reinterpret_cast<uintptr_t>(B.api)!=L.Bias+Rva)return VR_STATUS_PROVENANCE;
        if(Storage==72){Profile=EQualifiedNormalProfile2::Legacy72;return VR_STATUS_OK;}
        if(!CurrentPrivateRecord(R)||Storage>UINTPTR_MAX-(L.Bias+Rva))return VR_STATUS_ABI_INCOMPATIBLE;
        // This exact Q3 source edge already established ALL ordinary/private
        // target types, API==candidate RVA and WHOLE216 original/initialised RO
        // bounds under genuine continuous O/common-GT lifetime. No pointer/map/
        // magic check below grants those premises; no foreign arena is read.
        const auto& Actual=*reinterpret_cast<const VrPrivateOwnerApiObject1*>(B.api);
        const VrPrivateOwnerTail1& T=Actual.Private;
        if(Actual.Public.size!=72||Actual.Public.version!=2||T.Magic[0]!='V'||T.Magic[1]!='R'
            ||T.Magic[2]!='P'||T.Magic[3]!='R'||T.Magic[4]!='I'||T.Magic[5]!='V'||T.Magic[6]!='1'||T.Magic[7]
            ||T.Size!=144||T.Version!=1||T.SignatureAbi!=1||T.Count!=14||T.Mask!=0x3fff)return VR_STATUS_ABI_INCOMPATIBLE;
        if(T.ResourceProfile==2&&T.ResourceCells==140)Profile=EQualifiedNormalProfile2::Current2;
        else if(T.ResourceProfile==1&&T.ResourceCells==12)Profile=EQualifiedNormalProfile2::Historical1;
        else return VR_STATUS_ABI_INCOMPATIBLE;
        return VR_STATUS_OK;
    }
    static bool SameProtectedPath(const char* Actual,const char* Expected)
    {
        if(Actual==nullptr||Expected==nullptr)return false;
        for(uint32_t I=0;I<VR_Q_PATH_BYTES;++I)
        { if(Actual[I]!=Expected[I])return false;if(Actual[I]==0)return I!=0; }
        return false;
    }
    static void Unlock(FPassiveDescriptor2& D)
    { __atomic_store_n(&D.Lock,0u,__ATOMIC_RELEASE); }
    static bool TryLock(FPassiveDescriptor2& D)
    {
        uint32_t Expected=0;
        return __atomic_compare_exchange_n(&D.Lock,&Expected,1u,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);
    }
    static bool EmptyInline(const FPassiveDescriptor2& D)
    {
        const VrInlineEscrow2& E=D.Escrow;
        return E.size==sizeof(E)&&E.version==VR_OWNER_VERSION&&E.disposition==VR_DISPOSITION_NONE
            &&E.self_owned==0&&E.old_state==VR_OLD_NONE&&E.close_attempted==0
            &&E.reserved32==0&&E.reserved64[0]==0&&E.reserved64[1]==0
            &&IsEmptyPin(E.self_pin)&&IsEmptyPin(E.old_pin)
            &&E.expected_old.hi==0&&E.expected_old.lo==0&&E.expected_old_epoch==0;
    }
    static VrStatus LocalConsistencyLocked(FPassiveDescriptor2& D)
    {
        if(D.Reserved||D.Reserved2||D.ArenaJournalReserved||D.ControlBound>1
            ||D.ActivationGeneration==0||D.Escrow.phase>VR_PHASE_RELEASE_UNCERTAIN
            ||D.InlinePoisonReason>VR_POISON_NATIVE_INVARIANT)return VR_STATUS_ABI_INCOMPATIBLE;
        if((D.Owner==nullptr)!=(D.Api==nullptr)|| (D.ControlBound==1&&D.ControlProcess==0))return VR_STATUS_ABI_INCOMPATIBLE;
        const VrInlineEscrow2& E=D.Escrow;
        if(E.phase!=VR_PHASE_INVALID&&(E.size!=sizeof(E)||E.version!=VR_OWNER_VERSION
            ||E.disposition>VR_DISPOSITION_UNCERTAIN||E.self_owned>1||E.old_state>VR_OLD_IN_FLIGHT
            ||E.close_attempted>1||E.reserved32||E.reserved64[0]||E.reserved64[1]))return VR_STATUS_ABI_INCOMPATIBLE;
        if(D.ArenaJournalState>ArenaJournalUncertain)return VR_STATUS_ABI_INCOMPATIBLE;
        if(D.Escrow.phase==VR_PHASE_RELEASE_UNCERTAIN
            ||(D.Escrow.phase==VR_PHASE_POISONED&&D.Owner==nullptr))return VR_STATUS_RELEASE_UNCERTAIN;
        if(D.Escrow.phase==VR_PHASE_POISONED||D.InlinePoisonReason!=VR_POISON_NONE)return VR_STATUS_POISONED;
        if(D.Escrow.phase==VR_PHASE_PREPARED||D.Escrow.phase==VR_PHASE_ESCROW
            ||D.Escrow.phase==VR_PHASE_CLOSE_IN_FLIGHT)return VR_STATUS_BUSY;
        if(!IsJournalAbsent(D))return VR_STATUS_ABI_INCOMPATIBLE;
        if(D.Escrow.phase==VR_PHASE_INVALID)
        {
            return D.ControlBound==0&&D.ControlProcess==0&&D.Owner==nullptr&&D.Api==nullptr
                &&D.NextOwnerEpoch==0&&IsZeroBytes(&D.ControlThread,sizeof(D.ControlThread))
                &&IsZeroBytes(&D.Escrow,sizeof(D.Escrow))&&IsLeaseMirrorAbsent(D)
                ?VR_STATUS_OK:VR_STATUS_ABI_INCOMPATIBLE;
        }
        if(!EmptyInline(D))return VR_STATUS_ABI_INCOMPATIBLE;
        if(D.Escrow.phase==VR_PHASE_UNOWNED||D.Escrow.phase==VR_PHASE_RETIRED)
            return D.Owner==nullptr&&D.Api==nullptr&&IsLeaseMirrorAbsent(D)?VR_STATUS_OK:VR_STATUS_ABI_INCOMPATIBLE;
        if(D.Escrow.phase!=VR_PHASE_ACTIVE&&D.Escrow.phase!=VR_PHASE_DORMANT
            &&D.Escrow.phase!=VR_PHASE_RETIRING)return VR_STATUS_ABI_INCOMPATIBLE;
        return D.Owner!=nullptr&&D.Api!=nullptr?VR_STATUS_OK:VR_STATUS_ABI_INCOMPATIBLE;
    }
    static VrStatus BindOrCheckControl(FPassiveDescriptor2& D,uint64_t Process,pthread_t Thread)
    {
        // Only this genuine source chain may publish the immutable binding.
        // Matching this native identity alone never appoints a caller.
        if(!TryLock(D))return VR_STATUS_BUSY;
        VrStatus Status=LocalConsistencyLocked(D);
        if(Status==VR_STATUS_OK&&D.ControlBound==0)
        {
            if(D.Escrow.phase!=VR_PHASE_INVALID)Status=VR_STATUS_UNBOUND_CONTROL;
            else
            {
                D.ControlProcess=Process;
                D.ControlThread=Thread;
                ClearCompletedInlineForPhase(D,VR_PHASE_UNOWNED);
                __atomic_store_n(&D.ControlBound,1u,__ATOMIC_RELEASE);
            }
        }
        if(Status==VR_STATUS_OK)
        {
            if(D.ControlProcess!=Process)Status=VR_STATUS_FORK_UNESTABLISHED;
            else if(pthread_equal(Thread,D.ControlThread)==0)Status=VR_STATUS_WRONG_THREAD;
        }
        Unlock(D);return Status;
    }
    static VrStatus CloseNativeReaders(FQReadLedger1& L,VrStatus Status)
    {
        // All reader-fd responsibility stays in this actual frame. No new open
        // follows uncertainty; known independent cells still close once.
        for(uint32_t Kind=FdSelfMaps;Kind!=0;--Kind)
            for(FQReadFd1& F:L.Cells)
                if(F.Kind==Kind&&F.Custody==FdKnown)
                    if(CloseReadFd1(L,F)!=VR_STATUS_OK)Status=VR_STATUS_RELEASE_UNCERTAIN;
        for(const FQReadFd1& F:L.Cells)
            if(F.Custody!=FdEmpty&&F.Custody!=FdClosed)Status=VR_STATUS_RELEASE_UNCERTAIN;
        return Status;
    }
    static VrStatus ValidateKnownSelfCredit(FPassiveDescriptor2& D,FAdmissionLocal4& C,const VrPin& Pin)
    {
        const VrImageId Self=NativeImageIdForOwnerOperations();
        if(Pin.size!=sizeof(Pin)||Pin.version!=VR_OWNER_VERSION||Pin.platform!=VR_PLATFORM_GNU_LINUX_X64
            ||Pin.kind!=VR_PIN_OWNED_GNU_DLOPEN||Pin.value==0||!SameImage(Pin.image,Self)
            ||Self.hi!=C.Discovery.SelfBias||Self.lo!=C.Discovery.SelfGeneration)return VR_STATUS_PROVENANCE;
        void* const Handle=reinterpret_cast<void*>(uintptr_t(Pin.value));
        C.Namespace=LM_ID_BASE;
        if(dlinfo(Handle,RTLD_DI_LMID,&C.Namespace)!=0)return VR_STATUS_PROVENANCE;
        if(C.Namespace!=LM_ID_BASE)return VR_STATUS_NAMESPACE_INELIGIBLE;
        C.HandleMap=nullptr;
        if(dlinfo(Handle,RTLD_DI_LINKMAP,&C.HandleMap)!=0||C.HandleMap==nullptr)return VR_STATUS_PROVENANCE;
        C.AnchorMap=nullptr;
        if(dladdr1(reinterpret_cast<const void*>(&VrPrivateAdmitCurrentControl1),&C.Info,&C.AnchorMap,RTLD_DL_LINKMAP)==0
            ||C.AnchorMap==nullptr||C.AnchorMap!=C.HandleMap||C.Info.dli_fbase==nullptr
            ||reinterpret_cast<uintptr_t>(C.Info.dli_fbase)!=Self.hi
            ||!SameProtectedPath(C.Info.dli_fname,C.Path))return VR_STATUS_PROVENANCE;
        // O/S is a genuine credit supplied by creating code; borrowed opaque
        // maps above are equality facts only and are never decoded or released.
        return VR_STATUS_OK;
    }
    static VrStatus PollNormalOwner(FQScratchStorage1& S,FAdmissionLocal4& C)
    {
        const VrBootstrap1& B=S.Candidates[C.Discovery.NormalOwnerRecord].Bootstrap;
        if(B.size!=sizeof(B)||B.version!=VR_BOOTSTRAP_VERSION||B.magic!=VR_BOOTSTRAP_MAGIC
            ||B.pointer_bits!=VR_POINTER_BITS||B.endian!=VR_ENDIAN_LITTLE||B.owner_abi!=VR_OWNER_VERSION
            ||(B.flags&~VR_BOOTSTRAP_FLAGS_MASK)||B.reserved[0]||B.reserved[1]
            ||B.image.hi==0||B.image.lo==0)return VR_STATUS_ABI_INCOMPATIBLE;
        if(B.flags&VR_POISONED)return B.owner==nullptr&&B.api==nullptr?VR_STATUS_RELEASE_UNCERTAIN:VR_STATUS_POISONED;
        if((B.flags&VR_CONTROL_BUSY)&&C.Discovery.NormalOwnerRecord!=C.Discovery.CurrentRecord)return VR_STATUS_BUSY;
        if(!(B.flags&VR_BOUND_CONTROL)||(B.flags&VR_RETIRED)||B.owner==nullptr||B.api==nullptr)return VR_STATUS_ABI_INCOMPATIBLE;
        // Q3 established the genuine whole ordinary API/source/O range first.
        // Own transient busy is not cleared here: the creating poll itself
        // freshly locks/revalidates, and own output is synthesized only later.
        if(B.api->size!=sizeof(VrOwnerApi2)||B.api->version!=VR_OWNER_VERSION
            ||B.api->query==nullptr||B.api->poll_retirement==nullptr||B.api->retire==nullptr
            ||B.api->attach_service==nullptr||B.api->detach_service==nullptr||B.api->submit==nullptr
            ||B.api->copy_result==nullptr||B.api->discard==nullptr)return VR_STATUS_ABI_INCOMPATIBLE;
        ZeroBytes(&C.Snapshot,sizeof(C.Snapshot));
        C.Snapshot.size=sizeof(C.Snapshot);C.Snapshot.version=VR_OWNER_VERSION;
        // One old creating try-lock and at most2 true zero-time retire observations.
        // Genuine old O/common-GT/normal-return protects the complete call graph.
        VrStatus Status=B.api->poll_retirement(B.owner,&C.Snapshot);
        if(Status!=VR_STATUS_OK&&Status!=VR_STATUS_PENDING&&Status!=VR_STATUS_POISONED)return Status;
        const VrSnapshot& Q=C.Snapshot;
        if(Q.size!=sizeof(Q)||Q.version!=VR_OWNER_VERSION||Q.reserved||Q.owner_epoch==0
            ||!SameImage(Q.image,B.image)||Q.occupied_slots>VR_MAX_SLOTS||Q.connector_leased>1
            ||Q.service_executing>1||Q.poison_reason>VR_POISON_NATIVE_INVARIANT
            ||(Q.state!=VR_STATE_ACTIVE&&Q.state!=VR_STATE_DORMANT&&Q.state!=VR_STATE_RETIRING&&Q.state!=VR_STATE_POISONED))return VR_STATUS_ABI_INCOMPATIBLE;
        if(Q.state==VR_STATE_POISONED||Q.poison_reason!=VR_POISON_NONE||Status==VR_STATUS_POISONED)return VR_STATUS_POISONED;
        if(Q.connector_leased||Q.service_executing||Q.service_epoch!=0||Q.state==VR_STATE_RETIRING)return VR_STATUS_OLD_OWNER_BUSY;
        return Status;
    }
    static void PublishCurrentBootstrap(FPassiveDescriptor2& D,VrBootstrap1& Out)
    {
        // Actual descriptor lock remains held; known normal own phase only.
        Out.magic=VR_BOOTSTRAP_MAGIC;Out.pointer_bits=VR_POINTER_BITS;Out.endian=VR_ENDIAN_LITTLE;
        Out.owner_abi=VR_OWNER_VERSION;Out.flags=VR_BOUND_CONTROL;
        Out.image=NativeImageIdForOwnerOperations();
        for(uint32_t I=0;I<32;++I)Out.build_id[I]=ProducerIdentity[I];
        Out.owner=D.Owner;Out.api=D.Api;
    }

    static VrStatus RevalidateLocal(FPassiveDescriptor2& D,FAdmissionLocal4& C)
    {
        // Called under the final already-held successor lock; never a getter
        // busy-bit strip. Revalidate actual current state/identity after census.
        VrStatus Status=LocalConsistencyLocked(D);
        if(Status!=VR_STATUS_OK)return Status;
        Status=ValidateDescriptorControl(D);if(Status!=VR_STATUS_OK)return Status;
        if(pthread_equal(pthread_self(),D.ControlThread)==0)return VR_STATUS_WRONG_THREAD;
        const VrImageId Actual=NativeImageIdForOwnerOperations();
        if(Actual.hi!=C.Discovery.SelfBias||Actual.lo!=C.Discovery.SelfGeneration)return VR_STATUS_STALE;
        if(Load32(D.ActivationDepth)!=1)return VR_STATUS_BUSY;
        return VR_STATUS_OK;
    }
    static VrStatus ReuseOwnNormalLocked(FPassiveDescriptor2& D,FQScratchStorage1& S,FAdmissionLocal4& C,VrBootstrap1& Output)
    {
        VrStatus Status=RevalidateLocal(D,C);if(Status!=VR_STATUS_OK)return Status;
        const VrBootstrap1& B=S.Candidates[C.Discovery.NormalOwnerRecord].Bootstrap;
        if(B.owner!=D.Owner||B.api!=D.Api||D.Api!=NativeApiForOwnerConstruction())return VR_STATUS_STALE;
        VrOwner* Owner=nullptr;
        Status=ValidateOwner(D,B.owner,ENativeRole2::Control,Owner);if(Status!=VR_STATUS_OK)return Status;
        if(Owner->OwnerEpoch!=C.Snapshot.owner_epoch)return VR_STATUS_STALE;
        if(Owner->Phase==VR_PHASE_POISONED||Owner->Connector.Poisoned)return VR_STATUS_POISONED;
        if(Owner->Phase!=VR_PHASE_ACTIVE&&Owner->Phase!=VR_PHASE_DORMANT)return VR_STATUS_OLD_OWNER_BUSY;
        if(Owner->Connector.Leased||Owner->Connector.ServiceCallExecuting||Owner->Service.Registered
            ||!ResourceCountsZero(*Owner)||!IsLeaseMirrorAbsent(D))return VR_STATUS_OLD_OWNER_BUSY;
        Status=ValidateKnownSelfCredit(D,C,Owner->OwnerPin);
        if(Status!=VR_STATUS_OK)return Status;
        PublishCurrentBootstrap(D,Output);
        return VR_STATUS_OK; // Existing O/arena/slots/generations unchanged, no S.
    }
    static VrStatus PrepareSuccessorLocked(FPassiveDescriptor2& D,FAdmissionLocal4& C)
    {
        VrStatus Status=RevalidateLocal(D,C);if(Status!=VR_STATUS_OK)return Status;
        if(D.Owner!=nullptr||D.Api!=nullptr||!IsLeaseMirrorAbsent(D))return VR_STATUS_OLD_OWNER_BUSY;
        if(D.Escrow.phase!=VR_PHASE_UNOWNED&&D.Escrow.phase!=VR_PHASE_RETIRED)return VR_STATUS_BUSY;
        // Refuse wrap before S/old Free. Never roll a generation back, including
        // after a no-handle/failed-allocation refusal or mapped reactivation.
        if(D.Escrow.transfer_generation==UINT64_MAX||D.NextOwnerEpoch==UINT64_MAX
            ||(D.Escrow.phase==VR_PHASE_RETIRED&&D.ActivationGeneration==UINT64_MAX))return VR_STATUS_CAPACITY;
        C.ResumePhase=D.Escrow.phase;
        if(D.Escrow.phase==VR_PHASE_RETIRED)++D.ActivationGeneration;
        ++D.Escrow.transfer_generation;
        ClearCompletedInlineForPhase(D,VR_PHASE_PREPARED);
        D.Escrow.disposition=VR_DISPOSITION_PREPARED;
        C.Discovery.SelfGeneration=D.ActivationGeneration;
        ZeroBytes(&C.Request,sizeof(C.Request));
        C.Request.size=sizeof(C.Request);C.Request.version=VR_OWNER_VERSION;
        C.Request.successor=NativeImageIdForOwnerOperations();
        D.Escrow.self_pin.platform=VR_PLATFORM_GNU_LINUX_X64;
        D.Escrow.self_pin.kind=VR_PIN_OWNED_GNU_DLOPEN;
        D.Escrow.self_pin.image=C.Request.successor;
        // value0/self_owned0 is protected preparation, never an observable credit.
        return VR_STATUS_OK;
    }

    static VrStatus AcquireSelfLocked(FPassiveDescriptor2& D,FAdmissionLocal4& C)
    {
        // The exact immutable absolute P/source/E selection theorem precedes
        // this operation. No borrowed map is cast into a loader handle.
        void* const Handle=dlopen(C.Path,RTLD_NOLOAD|RTLD_NOW|RTLD_LOCAL);
        if(Handle==nullptr)
        {
            ClearCompletedInlineForPhase(D,C.ResumePhase);
            return VR_STATUS_NATIVE_ERROR;
        }
        // FIRST stores after the genuine successful self acquisition. Header,
        // exact image/platform/kind/capacity were prepared before the call.
        D.Escrow.self_pin.value=static_cast<uint64_t>(reinterpret_cast<uintptr_t>(Handle));
        D.Escrow.self_owned=1;
        asm volatile("" : : : "memory");
        return ValidateKnownSelfCredit(D,C,D.Escrow.self_pin);
    }
    static VrStatus RetireAndCloseOldLocked(FPassiveDescriptor2& D,FQScratchStorage1& S,FAdmissionLocal4& C)
    {
        VrBootstrap1& B=S.Candidates[C.Discovery.NormalOwnerRecord].Bootstrap;
        // ALL actual old ordinary8/source/platform/returned-pin semantics were
        // qualified before S/free, including GNU owned-dlopen kind2. A legacy
        // record alone supplies no permission to retire. No tail is consulted.
        VrStatus Status=B.api->retire(B.owner,&C.Request,&C.Transfer);
        if(Status!=VR_STATUS_OK)
        {
            B.owner=nullptr;B.api=nullptr;
            C.Discovery.NormalOwnerRecord=UINT32_MAX;
            // No old O was transferred. A retains its real known O/poison,
            // including uncertain arena Free; B may unwind ONLY its own S.
            // Actual current old munmap failure returns POISONED14/no token;
            // preserve14 here, do not probe A or relabel it as an old return19.
            return Status;
        }
        // IRREVERSIBLE TRANSFER: no returned-token validation, helper, allocation
        // or relock intervenes between actual FULL return and durable O custody.
        const uint8_t* From=reinterpret_cast<const uint8_t*>(&C.Transfer.released_owner_pin);
        volatile uint8_t* To=reinterpret_cast<volatile uint8_t*>(&D.Escrow.old_pin);
        for(uint32_t I=0;I<sizeof(VrPin);++I)To[I]=From[I];
        D.Escrow.old_state=VR_OLD_OWNED;
        D.Escrow.disposition=VR_DISPOSITION_TRANSFERRED;
        D.Escrow.phase=VR_PHASE_ESCROW;
        asm volatile("" : : : "memory");
        // The conduit/old route is no longer operational; only passive escrow
        // owns O. Historical image/epoch scalars remain in the prepared record.
        B.owner=nullptr;B.api=nullptr;
        C.Discovery.NormalOwnerRecord=UINT32_MAX;
        ClearPin(C.Transfer.released_owner_pin);
        D.Escrow.close_attempted=1;
        D.Escrow.old_state=VR_OLD_IN_FLIGHT;
        D.Escrow.disposition=VR_DISPOSITION_ATTEMPTING;
        D.Escrow.phase=VR_PHASE_CLOSE_IN_FLIGHT;
        asm volatile("" : : : "memory");
        const int Closed=dlclose(reinterpret_cast<void*>(uintptr_t(D.Escrow.old_pin.value)));
        if(Closed!=0)
        {
            // Potentially consumed old O becomes historical U: no call, query,
            // retry or destructor release. The distinct known S remains owned.
            D.Escrow.old_state=VR_OLD_UNCERTAIN;
            D.Escrow.disposition=VR_DISPOSITION_UNCERTAIN;
            D.Escrow.phase=VR_PHASE_RELEASE_UNCERTAIN;
            return VR_STATUS_RELEASE_UNCERTAIN;
        }
        ClearPin(D.Escrow.old_pin);
        D.Escrow.old_state=VR_OLD_NONE;
        D.Escrow.disposition=VR_DISPOSITION_CONSUMED;
        D.Escrow.phase=VR_PHASE_ESCROW;
        return VR_STATUS_OK;
    }

    static VrStatus UnwindPreparedSelf(FPassiveDescriptor2& D,FAdmissionLocal4& C,VrStatus Refusal)
    {
        // Enter ONLY with B's arena/lease/reader fds known discharged and
        // no B-held old O/U responsibility. Never close S on supported S+old-U.
        // A separately retained O/poison after A failed Free is A's duty, not
        // B custody: original escrow2 still permits B's PREPARED abort/unwind.
        if(D.Escrow.old_state!=VR_OLD_NONE||!IsEmptyPin(D.Escrow.old_pin))return VR_STATUS_RELEASE_UNCERTAIN;
        if(D.Owner!=nullptr||D.Api!=nullptr||!IsJournalAbsent(D)||!IsLeaseMirrorAbsent(D))return VR_STATUS_RELEASE_UNCERTAIN;
        for(const FQReadFd1& F:C.Files.Cells)
            if(F.Custody!=FdEmpty&&F.Custody!=FdClosed)return VR_STATUS_RELEASE_UNCERTAIN;
        if(D.Escrow.self_owned!=1||D.Escrow.self_pin.value==0)return VR_STATUS_RELEASE_UNCERTAIN;
        // Preserve the real historical expected-old tuple ONLY after a known
        // successful old close; no-transfer/foreign-reuse has no such history.
        if(D.Escrow.disposition!=VR_DISPOSITION_CONSUMED)
        {
            D.Escrow.expected_old={0,0};D.Escrow.expected_old_epoch=0;
        }
        D.Escrow.close_attempted=1;
        D.Escrow.disposition=VR_DISPOSITION_ATTEMPTING;
        D.Escrow.phase=VR_PHASE_CLOSE_IN_FLIGHT;
        asm volatile("" : : : "memory");
        const int Closed=dlclose(reinterpret_cast<void*>(uintptr_t(D.Escrow.self_pin.value)));
        D.Escrow.self_owned=0; // FIRST post-call store: no possibly-consumed known-S claim.
        if(Closed!=0)
        {
            // OUTSIDE the mandatory qualified glibc/nonfinal-under-true-E
            // return0 theorem. Raw self_pin is HISTORICAL self-U ONLY. No new
            // credit/retry/probe/reopen. This descriptor may vanish if later E
            // is final; permanent external Core gating is therefore essential.
            // This is NOT the supported known-S+old-U persistence theorem.
            D.Escrow.disposition=VR_DISPOSITION_UNCERTAIN;
            D.Escrow.phase=VR_PHASE_RELEASE_UNCERTAIN;
            return VR_STATUS_RELEASE_UNCERTAIN;
        }
        ClearCompletedInlineForPhase(D,C.ResumePhase);
        return Refusal;
    }
    static VrStatus AdmitWithScratch(FPassiveDescriptor2& D,VrBootstrap1& Output)
    {
        FQScratchStorage1 Scratch; // The SOLE256256 native scratch, not initialized twice.
        FQPhase1 Phase {};
        FAdmissionLocal4 C {};
        C.Files.Cells[0].Fd=-1;C.Files.Cells[1].Fd=-1;
        C.Discovery.CurrentRecord=UINT32_MAX;C.Discovery.NormalOwnerRecord=UINT32_MAX;
        VrStatus Status=LoadCurrentAndCensus1(Scratch,Phase,C.Files,C.Discovery);
        Status=CloseNativeReaders(C.Files,Status);
        if(Status!=VR_STATUS_OK)return Status; // Every fd19 dominates; no further query/S.
        const uint32_t Count=Read32(Scratch.Catalogue+VR_Q_H_COUNT);
        if(Count==0||Count>VR_Q_RECORDS||C.Discovery.CurrentRecord>=Count
            ||(C.Discovery.NormalOwnerRecord!=UINT32_MAX&&C.Discovery.NormalOwnerRecord>=Count)
            ||C.Discovery.SelfBias==0||C.Discovery.SelfGeneration==0
            ||C.Discovery.Images>VR_Q_CENSUS_IMAGES||C.Discovery.Candidates>VR_Q_RECORDS)return VR_STATUS_PROVENANCE;
        const uint8_t* Current=CatalogueRecord(Scratch,C.Discovery.CurrentRecord);
        // ReadOriginal in the completed Q3 edge checked this CURRENT original
        // header2/140 before census. Full compiled source/receipt qualification,
        // not raw14/216, independently establishes the cold current Profile2.
        // NEVER read a cold current mapped tail to manufacture that premise.
        static_assert(VR_PRIVATE_RESOURCE_PROFILE==2&&VR_PRIVATE_RESOURCE_CELLS==140);
        if(!CurrentPrivateRecord(Current))return VR_STATUS_ABI_INCOMPATIBLE;
        EQualifiedNormalProfile2 NormalProfile=EQualifiedNormalProfile2::None;
        Status=ClassifyQualifiedNormalProfile2(Scratch,C,NormalProfile);
        if(Status!=VR_STATUS_OK)return Status;
        if(NormalProfile==EQualifiedNormalProfile2::Historical1)return VR_STATUS_OLD_OWNER_BUSY;
        C.Path=reinterpret_cast<const char*>(Scratch.Paths+uint64_t(C.Discovery.CurrentRecord)*VR_Q_PATH_BYTES);
        if(C.Discovery.NormalOwnerRecord!=UINT32_MAX)
        {
            Status=PollNormalOwner(Scratch,C);
            if(Status!=VR_STATUS_OK&&Status!=VR_STATUS_PENDING)return Status;
            if(C.Discovery.NormalOwnerRecord==C.Discovery.CurrentRecord)
            {
                if(NormalProfile!=EQualifiedNormalProfile2::Current2)return VR_STATUS_ABI_INCOMPATIBLE;
                if(!TryLock(D))return VR_STATUS_BUSY;
                Status=ReuseOwnNormalLocked(D,Scratch,C,Output);
                Unlock(D);return Status;
            }
            // Same-image O reuse preserves even pending slots under its fresh
            // role/lease check. A foreign pre-S path keeps the original worker
            // preflight: PENDING refuses here before acquiring any B S.
            if(Status!=VR_STATUS_OK)return Status;
            const uint8_t* Old=CatalogueRecord(Scratch,C.Discovery.NormalOwnerRecord);
            if(NormalProfile==EQualifiedNormalProfile2::Legacy72)
            {
                // Only the already-qualified original ordinary ABI2/source/
                // retirement protocol permits this legacy handoff.72/mask is
                // never permission, an absent tail never makes an image empty.
                if(Read32(Old+172)!=VR_Q_API_PREFIX_BYTES)return VR_STATUS_ABI_INCOMPATIBLE;
                if(C.Snapshot.occupied_slots!=0)return VR_STATUS_OLD_OWNER_BUSY;
            }
        }
        // This one final lock precedes S and remains continuously held through
        // retirement/FULL return/durable O publication/sole close/result/arena.
        // Prefix/Q3 read-only checks have already released their own locks.
        if(!TryLock(D))return VR_STATUS_BUSY;
        Status=PrepareSuccessorLocked(D,C);
        if(Status!=VR_STATUS_OK){Unlock(D);return Status;}
        if(C.Discovery.NormalOwnerRecord!=UINT32_MAX
            &&NormalProfile==EQualifiedNormalProfile2::Legacy72)
        {
            const VrBootstrap1& Old=Scratch.Candidates[C.Discovery.NormalOwnerRecord].Bootstrap;
            // Native source-profile conformance already prevalidates exact
            // platform/kind2/return semantics, not the eventual transferred bits.
            if(Old.image.hi==C.Request.successor.hi||C.Snapshot.owner_epoch==0)
            { ClearCompletedInlineForPhase(D,C.ResumePhase);Unlock(D);return VR_STATUS_STALE; }
            C.Request.expected_epoch=C.Snapshot.owner_epoch;
            D.Escrow.expected_old=Old.image;D.Escrow.expected_old_epoch=C.Request.expected_epoch;
            ZeroBytes(&C.Transfer,sizeof(C.Transfer));
            C.Transfer.size=sizeof(C.Transfer);C.Transfer.version=VR_OWNER_VERSION;
        }
        Status=AcquireSelfLocked(D,C);
        if(Status!=VR_STATUS_OK)
        {
            if(D.Escrow.self_owned==1)Status=UnwindPreparedSelf(D,C,Status);
            Unlock(D);return Status;
        }
        if(C.Discovery.NormalOwnerRecord!=UINT32_MAX
            &&NormalProfile==EQualifiedNormalProfile2::Current2)
        {
            // Cold B cannot reinterpret caller E/borrowed map as an owned
            // handle. This one actual B selfS proves BASE, then known unwind
            // under true E precedes returning genuine A's existing normal O.
            // Same-image O reuse above never acquires S. No foreign pin exists.
            Status=UnwindPreparedSelf(D,C,VR_STATUS_OK);
            if(Status==VR_STATUS_OK)
                CopyBytes(&Scratch.Candidates[C.Discovery.NormalOwnerRecord].Bootstrap,&Output,sizeof(Output));
            Unlock(D);return Status;
        }
        if(C.Discovery.NormalOwnerRecord!=UINT32_MAX)
        {
            Status=RetireAndCloseOldLocked(D,Scratch,C);
            if(Status!=VR_STATUS_OK)
            {
                if(D.Escrow.phase!=VR_PHASE_RELEASE_UNCERTAIN)Status=UnwindPreparedSelf(D,C,Status);
                Unlock(D);return Status;
            }
        }
        // All constructor checks/preparation precede its mmap. After obtaining
        // any mapping that helper immediately journals actual custody, including
        // address0, then performs only prepared initialization/publication.
        Status=NativeConstructPreparedOwner();
        if(Status==VR_STATUS_OK)PublishCurrentBootstrap(D,Output);
        else if(D.Escrow.phase==VR_PHASE_POISONED||D.Escrow.phase==VR_PHASE_RELEASE_UNCERTAIN
            ||D.ArenaJournalState==ArenaJournalUncertain||D.Owner!=nullptr||D.Api!=nullptr)
            Status=VR_STATUS_RELEASE_UNCERTAIN; // Retain all real known S/arena responsibility.
        else Status=UnwindPreparedSelf(D,C,Status);
        Unlock(D);return Status;
    }
} // private same-image implementation

[[gnu::used,gnu::visibility("hidden")]] VrStatus NativeAdmitCurrentControl1(uint32_t OutputBytes,VrBootstrap1* Output)
{
    // Caller owns the complete accessible nonaliasing extent and all source/
    // code/normal-return lifetimes. No pointer probe or caller-identity packet.
    if(Output==nullptr||OutputBytes<sizeof(VrBootstrap1)
        ||reinterpret_cast<uintptr_t>(Output)%alignof(VrBootstrap1)!=0)return VR_STATUS_INVALID_INPUT;
    Detail::ZeroBytes(Output,sizeof(*Output));
    Output->size=sizeof(*Output);Output->version=VR_BOOTSTRAP_VERSION;
    // Ordinary identity sampling is BEFORE any try-CAS; actual initialization/
    // physical UE GT/Core/H/E-or-O authority is the closed initiating-source
    // premise, not inferred from these identity values or a build identifier.
    pid_t Process=getpid();
    pthread_t Thread=pthread_self();
    // Opaque read/write native operands keep these actual samples before the
    // CAS, rather than relying on a const/pure pthread_self call's placement.
    // Same selected-profile source discipline as the preserved C2-A entry.
    asm volatile("" : "+r"(Process), "+r"(Thread) : : "memory");
    if(Process<=0)return VR_STATUS_NATIVE_ERROR;
    FPassiveDescriptor2& D=NativeDescriptorForOwnerOperations();
    if(__atomic_load_n(&D.ControlBound,__ATOMIC_ACQUIRE)==1
        &&D.ControlProcess!=static_cast<uint64_t>(Process))return VR_STATUS_FORK_UNESTABLISHED;
    uint32_t Expected=0;
    if(!__atomic_compare_exchange_n(&D.ActivationDepth,&Expected,1u,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return VR_STATUS_BUSY;
    // No public loader query, catalogue/census or pin operation precedes this
    // actual exclusive activation. No spinning/retry/rebinding/destructor path.
    VrStatus Status=BindOrCheckControl(D,static_cast<uint64_t>(Process),Thread);
    if(Status==VR_STATUS_OK)Status=AdmitWithScratch(D,*Output);
    __atomic_store_n(&D.ActivationDepth,0u,__ATOMIC_RELEASE);
    return Status;
}
} // VaRestNativeOwnerPrivate
#endif
