// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestNativeOwnerDiscovery.h"
#include "VaRestNativeOwnerInternal.h"
#include "VaRestNativeOwnerProducer.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <unistd.h>

namespace VaRestNativeOwnerPrivate::Discovery1
{
namespace
{
    using namespace Deployment1;
    constexpr uint32_t None=UINT32_MAX;
    using FGetter1=VrStatus (VR_CALL *)(uint32_t,VrBootstrap1*);
    static_assert(sizeof(FGetter1)==8 && __atomic_always_lock_free(4,nullptr));
    static_assert(sizeof(ElfW(Phdr))==56 && sizeof(ElfW(Addr))==8);
    struct FSelf1
    {
        VrImageId Image;
        VrOwner* Owner;
        const VrOwnerApi2* Api;
        uint32_t Phase, Reserved;
        uint64_t Process;
        pthread_t Thread;
    };
    static_assert(sizeof(FSelf1)==56);
    struct FCensus1
    {
        FQScratchStorage1* Scratch;
        const FSelf1* Self;
        FQDiscovery1* Result;
        uint64_t Page;
        uint32_t Count, Visits, SelfSeen;
        VrStatus Status;
    };
    struct FLoad1
    {
        FQFileIdentity1 Identity;
        Dl_info Info;
        FQReadFd1* Original;
        void* Opaque;
        uint32_t Count, Selected;
    };
    union FPhase1 { FLoad1 Load; FCensus1 Census; };
    static_assert(std::is_trivial_v<FPhase1> && sizeof(FPhase1)==112);
    static_assert(sizeof(FQPhase1)+sizeof(FPhase1)+sizeof(FSelf1)+sizeof(FQReadLedger1)<=1024);

    // Entire reachable callback/getter closure must lower without imports/TLS/
    // compiler helpers. Source loops/used do NOT certify that later artifact.
    static void Clear(void* P,uint32_t N)
    { volatile uint8_t* D=static_cast<volatile uint8_t*>(P);for(uint32_t I=0;I<N;++I)D[I]=0; }
    static void Copy(void* P,const void* Q,uint32_t N)
    { volatile uint8_t* D=static_cast<volatile uint8_t*>(P);const uint8_t* S=static_cast<const uint8_t*>(Q);for(uint32_t I=0;I<N;++I)D[I]=S[I]; }
    static bool Equal(const void* P,const void* Q,uint32_t N)
    { const uint8_t* A=static_cast<const uint8_t*>(P);const uint8_t* B=static_cast<const uint8_t*>(Q);uint8_t X=0;for(uint32_t I=0;I<N;++I)X|=A[I]^B[I];return X==0; }
    static bool Zero(const void* P,uint32_t N)
    { const uint8_t* A=static_cast<const uint8_t*>(P);uint8_t X=0;for(uint32_t I=0;I<N;++I)X|=A[I];return X==0; }
    static uint32_t U32(const uint8_t* P)
    { return uint32_t(P[0])|uint32_t(P[1])<<8|uint32_t(P[2])<<16|uint32_t(P[3])<<24; }
    static uint64_t U64(const uint8_t* P) { return uint64_t(U32(P))|uint64_t(U32(P+4))<<32; }
    static bool Add(uint64_t A,uint64_t B,uint64_t& O) { if(B>UINT64_MAX-A)return false;O=A+B;return true; }
    static bool Relative(uint64_t A,uint64_t N,uint64_t& E) { return N&&A<=UINT32_MAX&&Add(A,N,E)&&E<=UINT32_MAX; }
    static bool Native(uint64_t B,uint64_t A,uint64_t& O) { return Add(B,A,O)&&O<=UINTPTR_MAX; }
    static bool Extent(uint64_t B,uint64_t A,uint64_t N)
    { uint64_t E,X,Y;return Relative(A,N,E)&&Native(B,A,X)&&Native(B,E,Y); }
    static bool Round(uint64_t V,uint64_t Page,uint64_t& E)
    { if(!Page)return false;const uint64_t R=V%Page;return Add(V,R?Page-R:0,E); }
    static uint32_t Phdrs(const FQParsedLayout1& L) { return L.Counts&255u; }
    static const uint8_t* Record(const FQScratchStorage1& S,uint32_t I)
    { return S.Catalogue+256+I*448; }
    static bool Contains(const FQParsedLayout1& L,uint64_t A,uint64_t N,bool File,uint32_t Need,uint32_t Forbid)
    {
        uint64_t E;if(!Relative(A,N,E))return false;
        for(uint32_t I=0;I<Phdrs(L);++I)
        {
            const uint8_t* H=L.Phdrs[I];uint64_t End;
            if(U32(H)==PT_LOAD&&Add(U64(H+16),U64(H+(File?32:40)),End)
                &&A>=U64(H+16)&&E<=End&&(U32(H+4)&Need)==Need&&!(U32(H+4)&Forbid))return true;
        }
        return false;
    }
    static bool ReadOnly(const FQParsedLayout1& L,uint64_t A,uint64_t N,uint64_t Page)
    {
        if(!Contains(L,A,N,true,PF_R,0)||!Page)return false;
        if(Contains(L,A,N,true,PF_R,PF_W))return true;
        uint64_t E;if(!Relative(A,N,E))return false;
        // Completed actual RELRO/initialization and no later mprotect/remap are
        // independent genuine loader/source premises. PF_W alone never proves RO.
        // Conservatively require the WHOLE object inside protected full pages.
        for(uint32_t I=0;I<Phdrs(L);++I)
        {
            const uint8_t* H=L.Phdrs[I];uint64_t End;
            if(U32(H)==PT_GNU_RELRO&&Add(U64(H+16),U64(H+40),End)&&End<=UINT32_MAX
                &&A>=U64(H+16)-U64(H+16)%Page&&E<=End-End%Page)return true;
        }
        return false;
    }
    static VrStatus Mask(const uint8_t* R,uint32_t& M)
    {
        M=0;
        for(uint32_t I=0;I<14;++I)
        {
            const uint8_t* P=R+176+I*8;const uint32_t A=U32(P),N=U32(P+4);uint64_t E;
            if(!A&&!N)continue;
            if(!A||!Relative(A,N,E))return VR_STATUS_PROVENANCE;
            M|=1u<<I;
        }
        return M==0x01fe||M==0x01ff||M==0x3ffe||M==0x3fff?VR_STATUS_OK:VR_STATUS_ABI_INCOMPATIBLE;
    }
    static bool NonblockingSelfPhase(uint32_t P)
    { return P==VR_PHASE_UNOWNED||P==VR_PHASE_ACTIVE||P==VR_PHASE_DORMANT||P==VR_PHASE_RETIRED; }
    static bool DepthHeld()
    { return __atomic_load_n(&NativeDescriptorForOwnerOperations().ActivationDepth,__ATOMIC_ACQUIRE)==1; }
    static VrStatus ReadSelf(FSelf1& S)
    {
        FPassiveDescriptor2& D=NativeDescriptorForOwnerOperations();
        if(__atomic_load_n(&D.ActivationDepth,__ATOMIC_ACQUIRE)!=1)return VR_STATUS_BUSY;
        pid_t Process=getpid();pthread_t Thread=pthread_self();
        // Real identities precede the sole CAS even for const pthread_self.
        asm volatile("" : "+r"(Process), "+r"(Thread) : : "memory");
        if(Process<=0)return VR_STATUS_NATIVE_ERROR;
        uint32_t Expected=0;
        if(!__atomic_compare_exchange_n(&D.Lock,&Expected,1u,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return VR_STATUS_BUSY;
        // Root's exactlyONE read-only passive lock: ONLY Lock is written. No
        // OS/imported identity/loader/read operation occurs under this lock.
        VrStatus Status=VR_STATUS_OK;
        if(__atomic_load_n(&D.ActivationDepth,__ATOMIC_ACQUIRE)!=1)Status=VR_STATUS_BUSY;
        else if(D.ControlBound>1||D.Reserved||D.Reserved2||D.ArenaJournalReserved
            ||D.ActivationGeneration==0||(D.ControlBound==1&&D.ControlProcess==0)
            ||D.InlinePoisonReason>VR_POISON_NATIVE_INVARIANT
            ||D.Escrow.phase>VR_PHASE_RELEASE_UNCERTAIN
            ||(D.Escrow.phase!=VR_PHASE_INVALID&&(D.Escrow.size!=160||D.Escrow.version!=2
                ||D.Escrow.reserved32||D.Escrow.reserved64[0]||D.Escrow.reserved64[1])))Status=VR_STATUS_ABI_INCOMPATIBLE;
        else if(D.Escrow.phase==VR_PHASE_POISONED||D.Escrow.phase==VR_PHASE_RELEASE_UNCERTAIN)
            Status=!D.Owner&&!D.Api?VR_STATUS_RELEASE_UNCERTAIN:VR_STATUS_POISONED;
        else if(D.Escrow.phase==VR_PHASE_PREPARED||D.Escrow.phase==VR_PHASE_ESCROW
            ||D.Escrow.phase==VR_PHASE_CLOSE_IN_FLIGHT||D.Escrow.phase==VR_PHASE_RETIRING)Status=VR_STATUS_BUSY;
        else if(D.ControlBound==0)Status=VR_STATUS_UNBOUND_CONTROL;
        else if(!NonblockingSelfPhase(D.Escrow.phase)||D.InlinePoisonReason
            ||D.ArenaJournalState!=ArenaJournalAbsent||D.ArenaJournalPointer||D.ArenaJournalBytes
            ||(D.Owner==nullptr)!=(D.Api==nullptr)
            ||bool(D.Owner)!=(D.Escrow.phase==VR_PHASE_ACTIVE||D.Escrow.phase==VR_PHASE_DORMANT))Status=VR_STATUS_ABI_INCOMPATIBLE;
        else
        {
            S.Image=NativeImageIdForOwnerOperations();S.Owner=D.Owner;S.Api=D.Api;
            S.Phase=D.Escrow.phase;S.Reserved=0;S.Process=D.ControlProcess;S.Thread=D.ControlThread;
        }
        __atomic_store_n(&D.Lock,0u,__ATOMIC_RELEASE);
        if(Status!=VR_STATUS_OK)return Status;
        // Descriptor is unlocked BEFORE proper pthread comparison/first query.
        if(static_cast<uint64_t>(Process)!=S.Process)return VR_STATUS_FORK_UNESTABLISHED;
        if(pthread_equal(Thread,S.Thread)==0)return VR_STATUS_WRONG_THREAD;
        if(!S.Image.hi||!S.Image.lo)return VR_STATUS_ABI_INCOMPATIBLE;
        return VR_STATUS_OK;
    }
    static VrStatus Name(FQScratchStorage1& S,const char* P,uint32_t& N)
    {
        N=0;if(!P)return VR_STATUS_PROVENANCE;
        for(uint32_t I=0;I<4096;++I)
        { const uint8_t C=static_cast<uint8_t>(P[I]);S.Io[I]=C;if(!C){N=I+1;return VR_STATUS_OK;} }
        return VR_STATUS_SCAN_LIMIT;
    }
    static uint32_t Find(const FQScratchStorage1& S,uint32_t Count,uint32_t N)
    {
        uint32_t Low=0,High=Count;
        // At most6 comparisons for32 cells. No new path or candidate buffer.
        for(uint32_t Step=0;Step<6&&Low<High;++Step)
        {
            const uint32_t Mid=Low+(High-Low)/2;const uint8_t* P=S.Paths+Mid*4096;
            int Order=0;
            for(uint32_t I=0;I<4096;++I)
            { if(S.Io[I]!=P[I]){Order=S.Io[I]<P[I]?-1:1;break;}if(!P[I])break; }
            if(Order==0)return U32(Record(S,Mid)+432)==N?Mid:None;
            if(Order<0)High=Mid;else Low=Mid+1;
        }
        return None;
    }
    static bool InfoContains(const dl_phdr_info& Info,uint64_t A,uint64_t N,uint32_t Need,uint32_t Forbid)
    {
        uint64_t E;if(!Relative(A,N,E))return false;
        for(uint32_t I=0;I<Info.dlpi_phnum;++I)
        {
            const ElfW(Phdr)& H=Info.dlpi_phdr[I];uint64_t End;
            if(H.p_type==PT_LOAD&&Add(H.p_vaddr,H.p_filesz,End)&&A>=H.p_vaddr&&E<=End
                &&(H.p_flags&Need)==Need&&!(H.p_flags&Forbid))return true;
        }
        return false;
    }
    static VrStatus Notes(const dl_phdr_info& Info,const uint8_t* R,uint32_t& Family)
    {
        Family=0;uint32_t Bytes=0,Build=0;
        for(uint32_t I=0;I<Info.dlpi_phnum;++I)
        {
            const ElfW(Phdr)& H=Info.dlpi_phdr[I];if(H.p_type!=PT_NOTE)continue;
            if(H.p_filesz>65536-Bytes)return VR_STATUS_SCAN_LIMIT;
            Bytes+=uint32_t(H.p_filesz);if(!H.p_filesz)continue;
            uint64_t Base,End;
            if(!InfoContains(Info,H.p_vaddr,H.p_filesz,PF_R,PF_W)
                ||!Native(Info.dlpi_addr,H.p_vaddr,Base)||!Add(Base,H.p_filesz,End)||End>UINTPTR_MAX)return VR_STATUS_PROVENANCE;
            for(uint64_t At=0;At<H.p_filesz;)
            {
                if(H.p_filesz-At<12)return VR_STATUS_PROVENANCE;
                const uint8_t* P=reinterpret_cast<const uint8_t*>(uintptr_t(Base+At));
                const uint32_t Names=U32(P),Desc=U32(P+4),Type=U32(P+8);
                uint64_t NP,DP,Total;
                if(!Round(Names,4,NP)||!Round(Desc,4,DP)||!Add(12,NP,Total)||!Add(Total,DP,Total)||Total>H.p_filesz-At)return VR_STATUS_PROVENANCE;
                if(Names>=6&&Equal(P+12,"VaRest",6))
                {
                    if(Names!=7||Desc!=64||Total!=84||Type!=0x56524f31||++Family!=1
                        ||!Equal(P+12,"VaRest\0",7)||P[19]||!Equal(P+20,"VROwner1",8)
                        ||U32(P+28)!=1||U32(P+32)!=1||U32(P+36)!=1||U32(P+40)!=1||!Zero(P+76,8))return VR_STATUS_ABI_INCOMPATIBLE;
                    if(!R||U32(R)!=2||!Equal(P+44,R+24,32))return VR_STATUS_PROVENANCE;
                }
                else if(Names==4&&Type==NT_GNU_BUILD_ID&&Equal(P+12,"GNU\0",4))
                {
                    if(Desc==0||Desc>32)return VR_STATUS_SCAN_LIMIT;
                    if(++Build!=1)return VR_STATUS_PROVENANCE;
                    if(R&&(Desc!=U32(R+120)||!Equal(P+16,R+88,Desc)))return VR_STATUS_PROVENANCE;
                }
                At+=Total;
            }
        }
        if(R&&(Build!=1||(U32(R)==2?Family!=1:Family!=0)))return VR_STATUS_PROVENANCE;
        return VR_STATUS_OK;
    }
    static VrStatus Layout(const dl_phdr_info& Info,FQParsedLayout1& L,const uint8_t* R,uint64_t Page,bool Self)
    {
        if(Self&&(Phdrs(L)!=Info.dlpi_phnum||!Equal(L.Phdrs,Info.dlpi_phdr,uint32_t(Info.dlpi_phnum)*56)))return VR_STATUS_PROVENANCE;
        if(!Self)Clear(&L,sizeof(L));
        Copy(L.Phdrs,Info.dlpi_phdr,uint32_t(Info.dlpi_phnum)*56);
        uint32_t Loads=0;bool First=false;
        for(uint32_t I=0;I<Info.dlpi_phnum;++I)
        {
            const ElfW(Phdr)& H=Info.dlpi_phdr[I];uint64_t FileEnd,MemEnd;
            if(H.p_filesz>H.p_memsz||!Add(H.p_offset,H.p_filesz,FileEnd)||FileEnd>U64(R+16)
                ||(H.p_memsz&&!Relative(H.p_vaddr,H.p_memsz,MemEnd))
                ||(H.p_align>1&&((H.p_align&(H.p_align-1))||H.p_offset%H.p_align!=H.p_vaddr%H.p_align)))return VR_STATUS_ABI_INCOMPATIBLE;
            if(H.p_type!=PT_LOAD)continue;
            if(!H.p_memsz){if(H.p_filesz)return VR_STATUS_PROVENANCE;continue;}
            if(Loads==8)return VR_STATUS_SCAN_LIMIT;
            uint64_t End,FileRounded,NativeStart,NativeEnd;
            if(!H.p_filesz||H.p_flags&~7u||H.p_offset%Page!=H.p_vaddr%Page
                ||!Round(MemEnd,Page,End)||End>UINT32_MAX
                ||!Add(H.p_vaddr,H.p_filesz,FileRounded)||!Round(FileRounded,Page,FileRounded)||FileRounded>UINT32_MAX
                ||!Native(Info.dlpi_addr,H.p_vaddr-H.p_vaddr%Page,NativeStart)
                ||!Native(Info.dlpi_addr,End,NativeEnd)||!Native(Info.dlpi_addr,FileRounded,NativeEnd)
                ||!Extent(Info.dlpi_addr,H.p_vaddr,H.p_memsz))return VR_STATUS_ABI_INCOMPATIBLE;
            FQLoad1& O=L.Loads[Loads];O.Start=H.p_vaddr-H.p_vaddr%Page;O.End=End;
            O.FileOffset=H.p_offset-H.p_offset%Page;O.Flags=H.p_flags;O.Reserved=0;
            for(uint32_t J=0;J<Loads;++J)if(O.Start<L.Loads[J].End&&L.Loads[J].Start<O.End)return VR_STATUS_ABI_INCOMPATIBLE;
            if(O.Start==0)
            { if(First||H.p_offset||H.p_vaddr||!(H.p_flags&PF_R)||H.p_filesz<64)return VR_STATUS_ABI_INCOMPATIBLE;First=true; }
            ++Loads;
        }
        if(!First||!Loads)return VR_STATUS_ABI_INCOMPATIBLE;
        L.Counts=uint32_t(Info.dlpi_phnum)|(Loads<<8);L.Bias=Info.dlpi_addr;
        if(!ReadOnly(L,0,64,Page))return VR_STATUS_PROVENANCE;
        const uint8_t* E=reinterpret_cast<const uint8_t*>(uintptr_t(Info.dlpi_addr));
        uint64_t Table,End;
        if(!Equal(E,"\177ELF",4)||E[4]!=2||E[5]!=1||E[6]!=1
            ||(E[7]!=0&&E[7]!=3)||!Zero(E+8,8)||U32(E+48)
            ||U32(E+16)!=0x003e0003||U32(E+20)!=1
            ||E[52]!=64||E[53]||E[54]!=56||E[55]||uint32_t(E[56])+uint32_t(E[57])*256!=Info.dlpi_phnum
            ||!Relative(U64(E+32),uint64_t(Info.dlpi_phnum)*56,End)
            ||!ReadOnly(L,U64(E+32),uint64_t(Info.dlpi_phnum)*56,Page)
            ||!Native(Info.dlpi_addr,U64(E+32),Table)||Table!=reinterpret_cast<uintptr_t>(Info.dlpi_phdr))return VR_STATUS_PROVENANCE;
        L.RecordIndex=U32(R+4);return VR_STATUS_OK;
    }
    static VrStatus Ranges(const FQParsedLayout1& L,const uint8_t* R,uint64_t Page)
    {
        if(U32(R)==1)return VR_STATUS_OK;
        if(!Extent(L.Bias,U32(R+136),U32(R+140))||!Contains(L,U32(R+136),U32(R+140),true,PF_R|PF_X,0))return VR_STATUS_PROVENANCE;
        for(uint32_t I=1;I<=3;++I)
            if(!Extent(L.Bias,U32(R+136+I*8),U32(R+140+I*8))||!Contains(L,U32(R+136+I*8),U32(R+140+I*8),false,PF_R|PF_W,0))return VR_STATUS_PROVENANCE;
        const uint32_t Storage=U32(R+172);
        if(!Extent(L.Bias,U32(R+168),Storage)||!ReadOnly(L,U32(R+168),Storage,Page))return VR_STATUS_PROVENANCE;
        for(uint32_t I=0;I<14;++I)
            if(U32(R+176+8*I)&&(!Extent(L.Bias,U32(R+176+8*I),U32(R+180+8*I))
                ||!Contains(L,U32(R+176+8*I),U32(R+180+8*I),true,PF_R|PF_X,0)))return VR_STATUS_PROVENANCE;
        for(uint32_t I=0;I<8;++I)
            if(!Extent(L.Bias,U32(R+288+4*I),1)||!Contains(L,U32(R+288+4*I),1,true,PF_R|PF_X,0))return VR_STATUS_PROVENANCE;
        return VR_STATUS_OK;
    }
    template<class T> static bool Match(T Actual,uint64_t Address)
    { return Actual!=nullptr&&Actual==reinterpret_cast<T>(uintptr_t(Address)); }
    static bool PublicTarget(const VrOwnerApi2& A,uint32_t I,uint64_t P)
    {
        switch(I)
        {
        case 0:return Match(A.query,P);case 1:return Match(A.poll_retirement,P);
        case 2:return Match(A.attach_service,P);case 3:return Match(A.detach_service,P);
        case 4:return Match(A.submit,P);case 5:return Match(A.copy_result,P);
        case 6:return Match(A.discard,P);case 7:return Match(A.retire,P);
        default:return false;
        }
    }
    static bool PrivateTarget(const VrPrivateOwnerTail1& T,uint32_t I,uint64_t P)
    {
        switch(I)
        {
        case 0:return Match(T.VrPrivateAdmitCurrentControl1,P);
        case 1:return Match(T.VrPrivateServiceNeverCreated1,P);
        case 2:return Match(T.VrPrivateRegisterCurrentServiceThread1,P);
        case 3:return Match(T.VrPrivateServiceJoinObserved1,P);
        case 4:return Match(T.VrPrivateRequestStop1,P);
        case 5:return Match(T.VrPrivatePumpRegistered1,P);
        case 6:return Match(T.VrPrivatePumpEnter1,P);
        case 7:return Match(T.VrPrivatePumpLeave1,P);
        case 8:return Match(T.VrPrivateRequestDetachAfterJoin1,P);
        case 9:return Match(T.VrPrivateResourceReserve1,P);
        case 10:return Match(T.VrPrivateResourceObtained1,P);
        case 11:return Match(T.VrPrivateResourceNotObtained1,P);
        case 12:return Match(T.VrPrivateResourceClosed1,P);
        case 13:return Match(T.VrPrivateResourceUncertain1,P);
        default:return false;
        }
    }
    static VrStatus Bootstrap(FCensus1& C,FQParsedLayout1& L,const uint8_t* R,bool Self)
    {
        VrBootstrap1& B=L.Bootstrap;Clear(&B,sizeof(B));
        uint64_t Getter;
        if(!Native(L.Bias,U32(R+136),Getter))return VR_STATUS_PROVENANCE;
        const FGetter1 Get=reinterpret_cast<FGetter1>(uintptr_t(Getter));
        const VrStatus Status=Get(sizeof(B),&B);
        if(B.size!=112||B.version!=1)return VR_STATUS_ABI_INCOMPATIBLE;
        if(Status!=VR_STATUS_OK)
        {
            if(!Zero(reinterpret_cast<const uint8_t*>(&B)+8,104))return VR_STATUS_ABI_INCOMPATIBLE;
            return Status>=VR_STATUS_OK&&Status<=VR_STATUS_RELEASE_UNCERTAIN?Status:VR_STATUS_ABI_INCOMPATIBLE;
        }
        if(B.magic!=VR_BOOTSTRAP_MAGIC||B.pointer_bits!=64||B.endian!=1||B.owner_abi!=2
            ||B.flags&~uint32_t(VR_BOOTSTRAP_FLAGS_MASK)||B.reserved[0]||B.reserved[1]
            ||B.image.hi!=L.Bias||!B.image.lo||!Equal(B.build_id,R+24,32))return VR_STATUS_ABI_INCOMPATIBLE;
        // Normative blocker order: poison BEFORE busy BEFORE pointer class.
        if(B.flags&VR_POISONED)return !B.owner&&!B.api?VR_STATUS_RELEASE_UNCERTAIN:VR_STATUS_POISONED;
        if(Self)
        {
            // The exact LOCKED self phase was snapshotted before any query.
            // Actual common-GT/Q4/H excludes new PREPARED/ESCROW/CLOSE writers
            // until this handling; otherwise this source interval is ineligible.
            // Source service poison remains visible and wins above. Q4 MUST
            // freshly lock/revalidate everything before S/irreversible/self reuse.
            if(!(B.flags&VR_BOUND_CONTROL)||!(B.flags&VR_CONTROL_BUSY)||B.image.lo!=C.Self->Image.lo
                ||B.owner!=C.Self->Owner||B.api!=C.Self->Api
                ||!NonblockingSelfPhase(C.Self->Phase)
                ||bool(B.flags&VR_RETIRED)!=(C.Self->Phase==VR_PHASE_RETIRED))return VR_STATUS_BUSY;
        }
        else if(B.flags&VR_CONTROL_BUSY)return VR_STATUS_BUSY;
        if((B.owner==nullptr)!=(B.api==nullptr))return VR_STATUS_ABI_INCOMPATIBLE;
        if(!B.owner){Clear(&B,sizeof(B));return VR_STATUS_OK;}
        if(B.flags&VR_RETIRED||!(B.flags&VR_BOUND_CONTROL)
            ||reinterpret_cast<uintptr_t>(B.owner)%8||reinterpret_cast<uintptr_t>(B.api)%8)return VR_STATUS_ABI_INCOMPATIBLE;
        uint32_t Inventory=0;const VrStatus Shape=Mask(R,Inventory);if(Shape!=VR_STATUS_OK)return Shape;
        const uint32_t Storage=U32(R+172);
        if(Storage!=72&&(Storage!=216||Inventory!=0x3fff))return VR_STATUS_ABI_INCOMPATIBLE;
        uint64_t Api;
        if(!Native(L.Bias,U32(R+168),Api)||reinterpret_cast<uintptr_t>(B.api)!=Api
            ||!Extent(L.Bias,U32(R+168),Storage)||!ReadOnly(L,U32(R+168),Storage,C.Page))return VR_STATUS_PROVENANCE;
        // Genuine full old/current ordinary8 source/receipt/original/initialized
        // RO/O qualification precedes this FIRST table read, even for legacy72.
        // These references/shape checks do not supply those source premises.
        const VrOwnerApi2& Public=*B.api;
        if(Public.size!=72||Public.version!=2)return VR_STATUS_ABI_INCOMPATIBLE;
        for(uint32_t I=0;I<8;++I)
        { uint64_t A;if(!Native(L.Bias,U32(R+288+4*I),A)||!PublicTarget(Public,I,A))return VR_STATUS_PROVENANCE; }
        if(Storage==72)
        {
            // Preserve the sole genuinely qualified ordinary legacy owner for
            // original query/poll/retire policy ONLY. Missing tail is neither
            // owner-free nor permission to retire/replace/reuse its private API.
            // No byte72, private function, extra copy/pin/array is touched here.
            if(C.Result->NormalOwnerRecord!=None)return VR_STATUS_OLD_OWNER_BUSY;
            C.Result->NormalOwnerRecord=L.RecordIndex;return VR_STATUS_OK;
        }
        // Only full14/216 reaches the typed tail. WHOLE216 initialized RO range
        // and genuine normal O have already been established above, BEFORE72.
        const VrPrivateOwnerApiObject1& O=*reinterpret_cast<const VrPrivateOwnerApiObject1*>(B.api);
        static constexpr char Magic[8]={'V','R','P','R','I','V','1',0};
        const VrPrivateOwnerTail1& T=O.Private;
        if(!Equal(T.Magic,Magic,8)||T.Size!=144||T.Version!=1||T.SignatureAbi!=1
            ||T.Count!=14||T.Mask!=Inventory
            ||!((T.ResourceProfile==2&&T.ResourceCells==140)||(T.ResourceProfile==1&&T.ResourceCells==12)))return VR_STATUS_ABI_INCOMPATIBLE;
        for(uint32_t I=0;I<14;++I)
        { uint64_t A;if(!Native(L.Bias,U32(R+176+8*I),A)||!PrivateTarget(T,I,A))return VR_STATUS_PROVENANCE; }
        // Actual Profile1/216 stays a qualified ordinary-only BLOCKER. Q4
        // classifies the genuinely O-protected header; it cannot R1-reuse this
        // historical private profile or infer owner-free from its incompatibility.
        if(C.Result->NormalOwnerRecord!=None)return VR_STATUS_OLD_OWNER_BUSY;
        C.Result->NormalOwnerRecord=L.RecordIndex;
        return VR_STATUS_OK;
    }
    static void ClearRoutes(FQScratchStorage1&);
    static int Stop(FCensus1& C,VrStatus S)
    {
        // No callable route survives ANY failed callback, including a normal
        // owner observed earlier in this same traversal. Never wait for a later
        // caller to clear refused/cold/inline/retired candidate addresses.
        ClearRoutes(*C.Scratch);C.Result->NormalOwnerRecord=None;C.Status=S;return 1;
    }
    static int Census(dl_phdr_info* Info,size_t Size,void* User)
    {
        FCensus1& C=*static_cast<FCensus1*>(User);
        if(C.Status!=VR_STATUS_OK)return 1;
        if(C.Visits==4096)return Stop(C,VR_STATUS_SCAN_LIMIT);
        ++C.Visits;
        if(!Info||Size<offsetof(dl_phdr_info,dlpi_phnum)+sizeof(Info->dlpi_phnum)
            ||!Info->dlpi_phdr)return Stop(C,VR_STATUS_PROVENANCE);
        if(!Info->dlpi_phnum||Info->dlpi_phnum>16)return Stop(C,VR_STATUS_SCAN_LIMIT);
        FQScratchStorage1& S=*C.Scratch;uint32_t NameBytes;
        VrStatus Status=Name(S,Info->dlpi_name,NameBytes);if(Status!=VR_STATUS_OK)return Stop(C,Status);
        const uint32_t Index=Find(S,C.Count,NameBytes);
        const uint8_t* R=Index==None?nullptr:Record(S,Index);uint32_t Family;
        Status=Notes(*Info,R,Family);if(Status!=VR_STATUS_OK)return Stop(C,Status);
        const uint32_t Class=R?U32(R):0;
        const bool Self=uint64_t(Info->dlpi_addr)==C.Self->Image.hi;
        if(Self&&(Index!=C.Result->CurrentRecord||!Family))return Stop(C,VR_STATUS_PROVENANCE);
        // Bounded dedup: <=8,386,560 prior-image comparisons for4096 visits,
        // no hash pool/foreign pin. Inconsistent same-record/bias facts refuse.
        for(uint32_t I=0;I<C.Result->Images;++I)
        {
            const FQImage1& Old=S.Images[I];
            if(Old.Bias==uint64_t(Info->dlpi_addr))
            {
                if(Old.Classification!=Class||Old.RecordIndex!=Index)return Stop(C,VR_STATUS_PROVENANCE);
                if(R&&(Phdrs(S.Candidates[Index])!=Info->dlpi_phnum
                    ||!Equal(S.Candidates[Index].Phdrs,Info->dlpi_phdr,uint32_t(Info->dlpi_phnum)*56)))return Stop(C,VR_STATUS_PROVENANCE);
                return 0;
            }
            if(Index!=None&&Old.RecordIndex==Index)return Stop(C,VR_STATUS_PROVENANCE);
        }
        if(C.Result->Images==4096)return Stop(C,VR_STATUS_SCAN_LIMIT);
        FQImage1& Image=S.Images[C.Result->Images++];Image.Bias=Info->dlpi_addr;Image.Classification=Class;Image.RecordIndex=Index;
        if(!R)return 0; // unrelated source-qualified nonowner, not lineage absence
        if(C.Result->Candidates==32)return Stop(C,VR_STATUS_SCAN_LIMIT);
        ++C.Result->Candidates;
        FQParsedLayout1& L=S.Candidates[Index];
        Status=Layout(*Info,L,R,C.Page,Self);if(Status!=VR_STATUS_OK)return Stop(C,Status);
        if(Self)++C.SelfSeen;
        if(Class==1){Clear(&L.Bootstrap,sizeof(L.Bootstrap));return 0;}
        Status=Ranges(L,R,C.Page);if(Status==VR_STATUS_OK)Status=Bootstrap(C,L,R,Self);
        if(Status!=VR_STATUS_OK){Clear(&L.Bootstrap,sizeof(L.Bootstrap));return Stop(C,Status);}
        return 0;
    }
    static VrStatus CloseFiles(FQReadLedger1& L,VrStatus Status)
    {
        // Only this same-source fresh ledger, never an arbitrary fd packet.
        // maps before original; historical uncertain fd is never touched again.
        for(uint32_t Kind=3;Kind;--Kind)
            for(FQReadFd1& F:L.Cells)
                if(F.Kind==Kind&&F.Custody==FdKnown)
                    if(CloseReadFd1(L,F)!=VR_STATUS_OK)Status=VR_STATUS_RELEASE_UNCERTAIN;
        for(const FQReadFd1& F:L.Cells)
            if(F.Custody!=FdEmpty&&F.Custody!=FdClosed)Status=VR_STATUS_RELEASE_UNCERTAIN;
        return Status;
    }
    static void ClearRoutes(FQScratchStorage1& S)
    { for(uint32_t I=0;I<32;++I)Clear(&S.Candidates[I].Bootstrap,sizeof(VrBootstrap1)); }
}

[[gnu::used,gnu::visibility("hidden")]] VrStatus LoadCurrentAndCensus1(
    Deployment1::FQScratchStorage1& Scratch,Deployment1::FQPhase1& Phase,
    Deployment1::FQReadLedger1& Files,FQDiscovery1& Result)
{
    using namespace Deployment1;
    // Actual same-image source graph owns nonaliasing accessible storage and its
    // sole scratch. No tuple/frame contents establish authority. No current caller.
    Clear(&Result,sizeof(Result));Result.CurrentRecord=None;Result.NormalOwnerRecord=None;
    ClearRoutes(Scratch);
    // Even a refused nonfresh use must never normalize historical reader19.
    for(const FQReadFd1& F:Files.Cells)if(F.Custody==FdUncertain)return VR_STATUS_RELEASE_UNCERTAIN;
    if(Files.NextOrdinal)return VR_STATUS_INVALID_INPUT;
    for(const FQReadFd1& F:Files.Cells)
        if(F.Fd!=-1||F.Custody||F.NativeError||F.Kind||F.AcquisitionOrdinal)return VR_STATUS_INVALID_INPUT;
    FSelf1 Self{};VrStatus Status=ReadSelf(Self);if(Status!=VR_STATUS_OK)return Status;
    FPhase1 Work{};
    Status=ReadCatalogue1(Scratch,Phase,Files,Work.Load.Count,Work.Load.Selected);
    if(Status!=VR_STATUS_OK)return CloseFiles(Files,Status);
    Work.Load.Opaque=nullptr;
    if(!DepthHeld())return CloseFiles(Files,VR_STATUS_BUSY);
    // FIRST loader query. Actual source Q4 ActivationDepth1 already held; own
    // read-only descriptor lock above has definitely been released on ALL paths.
    if(::dladdr1(reinterpret_cast<const void*>(&LoadCurrentAndCensus1),&Work.Load.Info,
        &Work.Load.Opaque,RTLD_DL_LINKMAP)==0||!Work.Load.Opaque
        ||reinterpret_cast<uintptr_t>(Work.Load.Info.dli_fbase)!=Self.Image.hi)
        return CloseFiles(Files,VR_STATUS_PROVENANCE);
    uint32_t NameBytes;
    Status=Name(Scratch,Work.Load.Info.dli_fname,NameBytes);
    if(Status!=VR_STATUS_OK)return CloseFiles(Files,Status);
    const uint32_t Current=Find(Scratch,Work.Load.Count,NameBytes);
    if(Current==None||!Equal(Record(Scratch,Current)+24,ProducerIdentity,32))return CloseFiles(Files,VR_STATUS_PROVENANCE);
    const uint8_t* R=Record(Scratch,Current);uint32_t Inventory=0;
    Status=Mask(R,Inventory);
    if(Status!=VR_STATUS_OK||U32(R)!=2||Inventory!=0x3fff||U32(R+172)!=216)
        return CloseFiles(Files,Status==VR_STATUS_OK?VR_STATUS_ABI_INCOMPATIBLE:Status);
    Result.SelfBias=Self.Image.hi;Result.SelfGeneration=Self.Image.lo;Result.CurrentRecord=Current;
    Work.Load.Original=nullptr;
    Status=ReadOriginal1(Scratch,Phase,Files,Current,Scratch.Candidates[Current],Work.Load.Identity,Work.Load.Original);
    if(Status==VR_STATUS_OK)Status=ObserveOwnedMapping1(Scratch,Phase,Files,*Work.Load.Original,
        Work.Load.Identity,Scratch.Candidates[Current],Self.Image.hi);
    // All native readers retire BEFORE the callback;19 dominates and stops any
    // further file open/census. Caller original1+our original/maps2 is max3.
    Status=CloseFiles(Files,Status);if(Status!=VR_STATUS_OK)return Status;
    // The actual protected current header/table are compared to the retained
    // original AFTER its complete maps association, BEFORE entering the census.
    // No PHDR query, file pass, pointer probe or duplicate array is introduced.
    const FQParsedLayout1& Own=Scratch.Candidates[Current];uint64_t Table;
    if(!ReadOnly(Own,0,64,Phase.PageBytes)
        ||!ReadOnly(Own,U64(Phase.ElfHeader+32),uint64_t(Phdrs(Own))*56,Phase.PageBytes)
        ||!Equal(reinterpret_cast<const void*>(uintptr_t(Self.Image.hi)),Phase.ElfHeader,64)
        ||!Native(Self.Image.hi,U64(Phase.ElfHeader+32),Table)
        ||!Extent(Self.Image.hi,U64(Phase.ElfHeader+32),uint64_t(Phdrs(Own))*56)
        ||!Equal(reinterpret_cast<const void*>(uintptr_t(Table)),Own.Phdrs,Phdrs(Own)*56))return VR_STATUS_PROVENANCE;
    const uint32_t Count=Work.Load.Count;
    // Start the trivial union's next source phase with no carried file/name/
    // identity pointer. No reader/borrowed path or authoritative packet escapes.
    Work.Census={&Scratch,&Self,&Result,Phase.PageBytes,Count,0,0,VR_STATUS_OK};
    if(!DepthHeld())return VR_STATUS_BUSY; // no new query after a lost interval
    const int Traversal=::dl_iterate_phdr(&Census,&Work.Census);
    Status=Work.Census.Status;
    if(Status==VR_STATUS_OK&&(Traversal!=0||Work.Census.SelfSeen!=1))Status=VR_STATUS_PROVENANCE;
    if(Status!=VR_STATUS_OK)
    { ClearRoutes(Scratch);Result.NormalOwnerRecord=None;return Status; }
    // Only ONE qualified ordinary normal owner's actual bootstrap survives in
    // its existing candidate cell under genuine continuous O/commonGT. Legacy72
    // grants ONLY original separately qualified ordinary policy, never tail/reuse.
    // No table/private binding is copied, and success grants no BASE/admission.
    return VR_STATUS_OK;
}
}
#endif
