// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspectorAllocation.h"

#include "CoreGlobals.h"
#include "HAL/PlatformMemory.h"
#include "HAL/UnrealMemory.h"
#include "Misc/AssertionMacros.h"

LLM_DEFINE_TAG(VaRest_Inspector);

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
bool TryReserve(std::atomic<int64>& Used, int64 Limit, int64 Amount, int64* OutUsed)
{
	if (Amount < 0 || Amount > Limit)
		return false;
	int64 Seen = Used.load(std::memory_order_relaxed);
	do
	{
		if (Seen < 0 || Seen > Limit - Amount)
			return false;
	}
	while (!Used.compare_exchange_weak(Seen, Seen + Amount, std::memory_order_acq_rel, std::memory_order_relaxed));
	if (OutUsed)
		*OutUsed = Seen + Amount;
	return true;
}
bool AddChecked(int64& Sum, int64 Amount, int64 Limit)
{
	if (Amount < 0 || Sum < 0 || Amount > Limit || Sum > Limit - Amount)
		return false;
	Sum += Amount;
	return true;
}
void ObservePeak(std::atomic<int64>& Peak, int64 Value)
{
	int64 Seen = Peak.load(std::memory_order_relaxed);
	while (Seen < Value && !Peak.compare_exchange_weak(Seen, Value, std::memory_order_relaxed))
	{
	}
}
void AddSaturated(std::atomic<uint64>& Counter, uint64 Amount)
{
	uint64 Seen = Counter.load(std::memory_order_relaxed);
	uint64 Next;
	do
	{
		Next = Amount > MAX_uint64 - Seen ? MAX_uint64 : Seen + Amount;
	}
	while (!Counter.compare_exchange_weak(Seen, Next, std::memory_order_release, std::memory_order_relaxed));
}
FPendingAccount::FPendingAccount(int64 InCountLimit,int64 InByteLimit)
 : Local(true),CountLimit(InCountLimit),ByteLimit(InByteLimit), Count(Local.Get().Count), Bytes(Local.Get().Bytes), PeakCount(Local.Get().PeakCount), PeakBytes(Local.Get().PeakBytes), ExcerptReservations(Local.Get().ExcerptReservations), DroppedCount(Local.Get().DroppedCount), DroppedBytes(Local.Get().DroppedBytes), MetadataOmitted(Local.Get().MetadataOmitted), ExcerptsSkipped(Local.Get().ExcerptsSkipped), LossEpoch(Local.Get().LossEpoch)
{}
FPendingAccount::FPendingAccount(FReservationDomain& D,uint8 InBank,TRefCountPtr<FAllocationLifetime> InLifetime)
 : Local(false),CountLimit(D.Identity.Banks[InBank].CountLimit),ByteLimit(D.Identity.Banks[InBank].ByteLimit), Count(D.Pending.Count), Bytes(D.Pending.Bytes), PeakCount(D.Pending.PeakCount), PeakBytes(D.Pending.PeakBytes), ExcerptReservations(D.Pending.ExcerptReservations), DroppedCount(D.Pending.DroppedCount), DroppedBytes(D.Pending.DroppedBytes), MetadataOmitted(D.Pending.MetadataOmitted), ExcerptsSkipped(D.Pending.ExcerptsSkipped), LossEpoch(D.Pending.LossEpoch),Lifetime(MoveTemp(InLifetime)),Domain(&D),Bank(InBank)
{ Domain->AddRole(Bank,uint8(EBankRole::PendingPayload),false,1); }
FPendingAccount::~FPendingAccount()
{
 if (!Domain) { check(Count.load()==0 && Bytes.load()==0 && ExcerptReservations.load()==0); }
 else
 {
  check(Domain->BankRoles[Bank][uint8(EBankRole::PendingTicket)].load()==0 && Domain->BankRoles[Bank][uint8(EBankRole::PendingBaseline)].load()==0);
  Domain->AddRole(Bank,uint8(EBankRole::PendingPayload),false,-1);
 }
}
bool FPendingAccount::ReserveBytes(int64 Charge,int64& OutBytes)
{
 if (!Domain) return TryReserve(Bytes,ByteLimit,Charge,&OutBytes);
 if (!Domain->Reserve(ELedgers::Pending,Charge)) return false;
 OutBytes=Bytes.fetch_add(Charge,std::memory_order_acq_rel)+Charge; return true;
}
void FPendingAccount::ReleaseBytes(int64 Charge)
{
 Bytes.fetch_sub(Charge,std::memory_order_acq_rel);
 if (Domain) Domain->Release(ELedgers::Pending,Charge);
}
FPendingTicket::FPendingTicket(FPendingTicket&& Other) noexcept
	: Account(MoveTemp(Other.Account))
	, Charge(Other.Charge)
{
	Other.Charge = 0;
}
FPendingTicket& FPendingTicket::operator=(FPendingTicket&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Account = MoveTemp(Other.Account);
		Charge = Other.Charge;
		Other.Charge = 0;
	}
	return *this;
}
FPendingTicket::~FPendingTicket()
{
	Reset();
}
TOptional<FPendingTicket> FPendingTicket::Acquire(const TSharedRef<FPendingAccount, ESPMode::ThreadSafe>& InAccount, int64 InCharge, bool* OutCountRefused)
{
	if (OutCountRefused)
		*OutCountRefused = false;
	int64 ReservedCount = 0, ReservedBytes = 0;
	if (!TryReserve(InAccount->Count, InAccount->CountLimit, 1, &ReservedCount))
	{
		if (OutCountRefused)
			*OutCountRefused = true;
		return {};
	}
	ObservePeak(InAccount->PeakCount, ReservedCount);
	if (!InAccount->ReserveBytes(InCharge,ReservedBytes))
	{
		InAccount->Count.fetch_sub(1, std::memory_order_acq_rel);
		return {};
	}
	ObservePeak(InAccount->PeakBytes, ReservedBytes);
	FPendingTicket Ticket;
	if (InAccount->Domain) InAccount->Domain->AddRole(InAccount->Bank,uint8(EBankRole::PendingTicket),false,1);
	Ticket.Account = InAccount;
	Ticket.Charge = InCharge;
	return TOptional<FPendingTicket>(MoveTemp(Ticket));
}
void FPendingTicket::Reset()
{
	if (Account.IsValid())
	{
		FRootRegionLease Foreign;
  if (Account->Domain) Foreign=FRootRegionLease(Account->Lifetime->Region.PinRoot(),*Account->Domain,Account->Bank,uint8(EBankRole::ForeignAccount),false);
		Account->ReleaseBytes(Charge);
		Account->Count.fetch_sub(1, std::memory_order_acq_rel);
		if (Account->Domain) Account->Domain->AddRole(Account->Bank,uint8(EBankRole::PendingTicket),false,-1);
		Charge = 0;
		Account.Reset();
	}
}
FPendingBaseline::FPendingBaseline(FPendingBaseline&& Other) noexcept
	: Account(MoveTemp(Other.Account))
	, Charge(Other.Charge)
{
	Other.Charge = 0;
}
FPendingBaseline& FPendingBaseline::operator=(FPendingBaseline&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Account = MoveTemp(Other.Account);
		Charge = Other.Charge;
		Other.Charge = 0;
	}
	return *this;
}
FPendingBaseline::~FPendingBaseline()
{
	Reset();
}
TOptional<FPendingBaseline> FPendingBaseline::Acquire(const TSharedRef<FPendingAccount, ESPMode::ThreadSafe>& InAccount, int64 InCharge)
{
	int64 ReservedBytes = 0;
	if (!InAccount->ReserveBytes(InCharge,ReservedBytes))
		return {};
	ObservePeak(InAccount->PeakBytes, ReservedBytes);
	FPendingBaseline Result;
	if (InAccount->Domain) InAccount->Domain->AddRole(InAccount->Bank,uint8(EBankRole::PendingBaseline),false,1);
	Result.Account = InAccount;
	Result.Charge = InCharge;
	return TOptional<FPendingBaseline>(MoveTemp(Result));
}
void FPendingBaseline::Reset()
{
	if (Account.IsValid())
	{
		FRootRegionLease Foreign;
  if (Account->Domain) Foreign=FRootRegionLease(Account->Lifetime->Region.PinRoot(),*Account->Domain,Account->Bank,uint8(EBankRole::ForeignAccount),false);
		Account->ReleaseBytes(Charge);
		if (Account->Domain) Account->Domain->AddRole(Account->Bank,uint8(EBankRole::PendingBaseline),false,-1);
		Charge = 0;
		Account.Reset();
	}
}
FAllocationAdapter::FAllocationAdapter(FAllocationCounters& InCounters, FSupportedAllocator InSupport)
	: Local(true),Probe(&InCounters),State(&Local.Get())
{ State->Support=InSupport; }
FAllocationAdapter::FAllocationAdapter(FAllocationCounters& P,FAdapterState& S,FSupportedAllocator Support)
 : Local(false),Probe(&P),State(&S) { State->Support=Support; }
FAdapterImage FAllocationAdapter::ExportState() const { return {State->Support,State->CalibrationAttempted,State->Failed.load(std::memory_order_acquire)}; }
FSupportedAllocator FAllocationAdapter::GetSupport() const
{
	return State->Failed.load(std::memory_order_acquire) ? FSupportedAllocator{} : State->Support;
}
bool FAllocationAdapter::Calibrate(std::atomic<int64>& StartupUsed, int64 StartupLimit, SIZE_T ControllerRequest, uint32 TypeAlignment)
{
	check(IsInGameThread());
	if (State->CalibrationAttempted)
		return false;
	State->CalibrationAttempted = true;
	if (State->Support.Family == FSupportedAllocator::EFamily::Unsupported || !State->Support.bVerifiedModuleNew ||
		!State->Support.bVerifiedNoInflatingProxy || State->Support.bCalibrationPassed || sizeof(void*) != 8 ||
		ControllerRequest == 0 || ControllerRequest > 1024 || TypeAlignment == 0 || TypeAlignment > 16 ||
		(TypeAlignment & (TypeAlignment - 1)) != 0 || __STDCPP_DEFAULT_NEW_ALIGNMENT__ > 16)
	{
		AddSaturated(Probe->UnsupportedAllocator);
		return false;
	}
	if (!TryReserve(StartupUsed, StartupLimit, 4096))
		return false;
	const uint32 OrdinaryAlignment = ControllerRequest <= 8 ? 8 : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
	LLM_SCOPE_BYTAG(VaRest_Inspector);
	void* Ptr = FMemory::Malloc(ControllerRequest, OrdinaryAlignment);
	const SIZE_T Actual = Ptr ? FMemory::GetAllocSize(Ptr) : 0;
	Probe->CalibrationRequest.store(int64(ControllerRequest));
	Probe->CalibrationAlignment.store(OrdinaryAlignment);
	Probe->CalibrationCapacity.store(int64(Actual));
	const int64 Live = Probe->LiveBacking.fetch_add(int64(Actual)) + int64(Actual);
	ObservePeak(Probe->PeakBacking, Live);
	FMemory::Free(Ptr);
	Probe->LiveBacking.fetch_sub(int64(Actual));
	StartupUsed.fetch_sub(4096, std::memory_order_acq_rel);
	if (!Ptr || Actual < ControllerRequest || Actual > 4096)
	{
		State->Failed.store(true, std::memory_order_release);
		AddSaturated(Probe->BoundFailures);
		ensureMsgf(false, TEXT("Inspector allocator calibration violated its certified bound"));
		return false;
	}
	State->Support.bCalibrationPassed = true;
	return true;
}
TOptional<SIZE_T> FAllocationAdapter::Bound(SIZE_T Request, uint32 Alignment) const
{
	const FSupportedAllocator Capability = GetSupport();
	if (!Capability.bCalibrationPassed || !Capability.bVerifiedModuleNew || !Capability.bVerifiedNoInflatingProxy ||
		Request == 0 || Request > SIZE_T(FNormalizedLimits::MaxMetadataBytes) || Alignment == 0 || Alignment > 16 ||
		(Alignment & (Alignment - 1)) != 0)
		return {};
	const auto& Constants = FPlatformMemory::GetConstants();
	if (Capability.Family == FSupportedAllocator::EFamily::Mimalloc200)
	{
		if (Constants.PageSize == 0 || Constants.PageSize > 65536 || (Constants.PageSize & (Constants.PageSize - 1)) != 0)
			return {};
		// Audited mimalloc2.0.0 alloc-aligned.c:17-59 adds at most15;
		// page-queue.c:55-94/init.c:52-69 bins fit the next power of two.
		// page.c:783-815/os.c:98-108 round large blocks (here <2MiB)
		// to at most64KiB. The power-of-two bound covers that rounding.
		int64 Padded = int64(Request);
		if (!AddChecked(Padded, 15 + 8, MAX_int64))
			return {};
		SIZE_T Capacity = 64;
		while (Capacity < SIZE_T(Padded))
		{
			if (Capacity > SIZE_T(MAX_int64) / 2)
				return {};
			Capacity *= 2;
		}
		return Capacity;
	}
	if (Capability.Family == FSupportedAllocator::EFamily::Binned2)
	{
		const SIZE_T Granularity = Constants.BinnedAllocationGranularity ? Constants.BinnedAllocationGranularity : Constants.BinnedPageSize;
		if (Granularity == 0 || Granularity > 65536 || (Granularity & (Granularity - 1)) != 0 || Constants.BinnedPageSize < 16)
			return {};
		// MallocBinned2.cpp:717-758 records the aligned OS allocation;
		// MallocBinnedCommon.h:896-922 returns that capacity. Small bins
		// are <=32752; the <=1024 controller proof has a4096 bound.
		if (Request <= 1024)
			return SIZE_T(4096);
		if (Request <= 65536)
			return SIZE_T(65536);
		const SIZE_T Multiple = Granularity < 16 ? 16 : Granularity;
		int64 Rounded = int64(Request);
		if (!AddChecked(Rounded, int64(Multiple - 1), MAX_int64))
			return {};
		return SIZE_T(Rounded) & ~(Multiple - 1);
	}
	return {};
}
void* FAllocationAdapter::AllocateReserved(SIZE_T Request, uint32 Alignment, SIZE_T ReservedBound)
{
	const auto Certified = Bound(Request, Alignment);
	if (!Certified.IsSet() || Certified.GetValue() != ReservedBound)
	{
		AddSaturated(Probe->UnsupportedAllocator);
		return nullptr;
	}
	LLM_SCOPE_BYTAG(VaRest_Inspector);
	void* Ptr = FMemory::Malloc(Request, Alignment);
	const SIZE_T Actual = Ptr ? FMemory::GetAllocSize(Ptr) : 0;
	if (!Ptr || Actual < Request || Actual > ReservedBound)
	{
		FMemory::Free(Ptr);
		State->Failed.store(true, std::memory_order_release);
		AddSaturated(Probe->BoundFailures);
		ensureMsgf(false, TEXT("Inspector allocation violated its certified bound"));
		return nullptr;
	}
	const int64 Live = Probe->LiveBacking.fetch_add(int64(Actual)) + int64(Actual);
	ObservePeak(Probe->PeakBacking, Live);
	return Ptr;
}
void FAllocationAdapter::Free(void* Ptr)
{
	if (!Ptr)
		return;
	const SIZE_T Actual = FMemory::GetAllocSize(Ptr);
	FMemory::Free(Ptr);
	Probe->LiveBacking.fetch_sub(int64(Actual), std::memory_order_acq_rel);
}
FAllocationCounters& FAllocationAdapter::Counters() const
{
	return *Probe;
}

namespace
{
std::atomic<uint64> NextDomainSerial{0};
uint64 Serial(std::atomic<uint64>& Counter)
{
 uint64 Seen=Counter.load();
 while (Seen!=MAX_uint64) if (Counter.compare_exchange_weak(Seen,Seen+1)) return Seen+1;
 return 0;
}
FAdapterImage CaptureAdapter(const FAdapterState& S) { return {S.Support,S.CalibrationAttempted,S.Failed.load()}; }
void RestoreAdapter(FAdapterState& S,const FAdapterImage& I) { S.Support=I.Support; S.CalibrationAttempted=I.CalibrationAttempted; S.Failed.store(I.Failed); }
}
bool FRootClaim::IsEmpty() const
{
 return Serial==0 && Reserved==0 && ObservedCapacity==0 && Request==0 && Alignment==0 && Ledgers==ELedgers::None && Kind==EClaimKind::RootBacking && State==EClaimState::Empty;
}
FReservationDomain::FReservationDomain()=default;
FReservationDomain::FReservationDomain(const FCapturePolicy& P,FSupportedAllocator S)
{
 Identity.DomainSerial=Serial(NextDomainSerial); Identity.PendingLedgerSerial=Identity.DomainSerial; Identity.StoreLedgerSerial=Identity.DomainSerial;
 Identity.Effective=P.Limits; Identity.Requested=P.Limits; Identity.PolicyBits=uint8(P.bHeaders)|(uint8(P.bBodies)<<1);
 Identity.AuthorityPhase=EAuthorityPhase::Active; BootstrapAdapter.Support=S; LifetimeAdapter.Support=S;
 if (!Identity.DomainSerial) Identity.StickyRefusal=true;
}
int64 FReservationDomain::PendingCap() const { return Identity.DomainPhase==EDomainPhase::Recreating ? FMath::Min(Identity.Effective.PendingBytes,Identity.Requested.PendingBytes) : Identity.Effective.PendingBytes; }
int64 FReservationDomain::StoreCap() const { return Identity.DomainPhase==EDomainPhase::Recreating ? FMath::Min(Identity.Effective.StoreBytes,Identity.Requested.StoreBytes) : Identity.Effective.StoreBytes; }
bool FReservationDomain::Reserve(ELedgers Ledgers,int64 Amount)
{
 if (Identity.AuthorityPhase!=EAuthorityPhase::Active || Amount<0 || Ledgers==ELedgers::None) return false;
 int64 P=0,S=0;
 const bool bP=(uint8(Ledgers)&1)!=0,bS=(uint8(Ledgers)&2)!=0;
 if (bP && !TryReserve(PendingTotal,PendingCap(),Amount,&P)) return false;
 if (bP) ObservePeak(PendingPeak,P);
 if (bS && !TryReserve(StoreTotal,StoreCap(),Amount,&S)) { if (bP) PendingTotal.fetch_sub(Amount); return false; }
 if (bS) ObservePeak(StorePeak,S);
 return true;
}
void FReservationDomain::Release(ELedgers L,int64 A)
{
 check(Identity.AuthorityPhase==EAuthorityPhase::Active && A>=0);
 if ((uint8(L)&1)!=0) { const int64 Old=PendingTotal.fetch_sub(A); check(Old>=A); }
 if ((uint8(L)&2)!=0) { const int64 Old=StoreTotal.fetch_sub(A); check(Old>=A); }
}
bool FReservationDomain::Enter(uint8 Bank)
{
 uint64 Seen=GateWord.load(std::memory_order_acquire);
 while (!(Seen&ClosedBit) && Seen<ClosedBit-1)
  if (GateWord.compare_exchange_weak(Seen,Seen+1,std::memory_order_acq_rel))
  {
   if (Bank==Identity.ActiveBankSlot && Identity.DomainPhase==EDomainPhase::Live) return true;
   GateWord.fetch_sub(1,std::memory_order_release); return false;
  }
 return false;
}
void FReservationDomain::Leave() { const uint64 Old=GateWord.fetch_sub(1,std::memory_order_release); check((Old&~ClosedBit)>0); }
void FReservationDomain::Close() { GateWord.fetch_or(ClosedBit,std::memory_order_acq_rel); }
bool FReservationDomain::Reopen(uint8 Bank)
{
 uint64 Seen=GateWord.load(std::memory_order_acquire);
 if (!(Seen&ClosedBit) || Identity.Closed || Bank>1 || AdmissionEpoch.load()==MAX_uint64) return false;
 if ((Seen&~ClosedBit)!=0 && Bank!=Identity.ActiveBankSlot) return false;
 if (Bank!=Identity.ActiveBankSlot) { Identity.ActiveBankSlot=Bank; Identity.ActiveBankSerial=Identity.Banks[Bank].Serial; }
 if (Identity.DomainPhase!=EDomainPhase::Live) { if ((Seen&~ClosedBit)!=0) return false; Identity.DomainPhase=EDomainPhase::Live; }
 AdmissionEpoch.fetch_add(1,std::memory_order_release);
 do { if (!(Seen&ClosedBit)) return false; }
 while (!GateWord.compare_exchange_weak(Seen,Seen&~ClosedBit,std::memory_order_acq_rel,std::memory_order_acquire));
 return true; // Preserve every admitted old-generation lease; no count reset.
}

bool FReservationDomain::ReserveClaim(uint8 Slot,EClaimKind Kind,SIZE_T Request,uint32 Alignment,int64 Charge,ELedgers L)
{
 if (Slot>=16 || !Claims[Slot].IsEmpty() || Request==0 || Request>SIZE_T(FNormalizedLimits::MaxMetadataBytes) || Alignment==0 || Alignment>16 || (Alignment&(Alignment-1)) || Identity.NextClaimSerial==MAX_uint64 || !Reserve(L,Charge)) return false;
 Claims[Slot]={++Identity.NextClaimSerial,Charge,0,Request,Alignment,L,Kind,EClaimState::Reserved}; return true;
}
void FReservationDomain::ObserveClaim(uint8 Slot,int64 Actual)
{
 check(Slot<16 && Claims[Slot].State==EClaimState::Reserved);
 Claims[Slot].ObservedCapacity=Actual; Claims[Slot].State=EClaimState::BackingLive;
}
void FReservationDomain::RetireClaim(uint8 Slot)
{
 check(Slot<16 && Claims[Slot].State!=EClaimState::Empty);
 FRootClaim& C=Claims[Slot]; C.State=EClaimState::Retired; Release(C.Ledgers,C.Reserved);
 // Exclusive detached ownership AFTER actual free/final weak; serial/cumulative cells stay intact.
 C=FRootClaim{};
}
bool FReservationDomain::ReserveScratch()
{
 if (ScratchLive || !Scratch.IsEmpty() || Identity.NextClaimSerial==MAX_uint64 || !Reserve(ELedgers::Both,4096)) return false;
 Scratch={++Identity.NextClaimSerial,4096,0,0,0,ELedgers::Both,EClaimKind::Scratch,EClaimState::Reserved}; ScratchLive=true; return true;
}
void FReservationDomain::RetireScratch() { check(ScratchLive); Release(ELedgers::Both,Scratch.Reserved); Scratch=FRootClaim{}; ScratchLive=false; }
void FReservationDomain::AddRole(int8 Bank,uint8 Role,bool bRoot,int32 Delta)
{
 check(Delta==1 || Delta==-1);
 auto& C=bRoot ? RootRoles[Role] : BankRoles[Bank][Role]; const int32 Old=C.fetch_add(Delta,std::memory_order_acq_rel); check(Old>=0 && (Delta>0 || Old>0));
}
bool FReservationDomain::RolesZero() const
{
 for (const auto& B:BankRoles) for (const auto& R:B) if (R.load(std::memory_order_acquire)) return false;
 for (const auto& R:RootRoles) if (R.load(std::memory_order_acquire)) return false;
 return true;
}
bool FReservationDomain::BankReusable(uint8 B) const
{
 if (B>1) return false;
 for (const auto& R:BankRoles[B]) if (R.load(std::memory_order_acquire)) return false;
 return true;
}
bool FRecreationStagingPermit::Valid(const FReservationDomain& D,uint8 Bank) const
{ return Guard.Domain()==&D && Bank==NextBank && D.Identity.StagingSerial==Serial && D.Identity.DomainPhase==EDomainPhase::Recreating && D.GateWord.load()==FReservationDomain::ClosedBit; }
FRootRegionLease::FRootRegionLease(TSharedPtr<FProductionRootBlock,ESPMode::ThreadSafe> R,FReservationDomain& D,int8 B,uint8 K,bool bR)
 : Root(MoveTemp(R)),Owner(&D),Bank(B),Role(K),bRootRole(bR) { check(Root); Owner->AddRole(Bank,Role,bRootRole,1); }
FRootRegionLease::FRootRegionLease(FRootRegionLease&& O) noexcept
 : Root(MoveTemp(O.Root)),Owner(O.Owner),Bank(O.Bank),Role(O.Role),bRootRole(O.bRootRole) { O.Owner=nullptr; }
FRootRegionLease& FRootRegionLease::operator=(FRootRegionLease&& O) noexcept
{
 if (this!=&O) { Reset(); Root=MoveTemp(O.Root); Owner=O.Owner; Bank=O.Bank; Role=O.Role; bRootRole=O.bRootRole; O.Owner=nullptr; } return *this;
}
FRootRegionLease::~FRootRegionLease() { Reset(); }
FRootRegionLease FRootRegionLease::Clone() const { check(Owner && Root); return FRootRegionLease(Root,*Owner,Bank,Role,bRootRole); }
void FRootRegionLease::Reset()
{
 if (!Owner) return;
#if WITH_DEV_AUTOMATION_TESTS
 if (!bRootRole && Role==uint8(EBankRole::ForeignAccount))
 {
  auto* Hooks=Owner->TeardownTestHooks.load(std::memory_order_acquire);
  if (Hooks && Hooks->Bank==Bank && Owner->BankRoles[Bank][uint8(EBankRole::ForeignAccount)].load(std::memory_order_acquire)==1 && Hooks->Armed.exchange(false,std::memory_order_acq_rel))
  { check(Hooks->Ready && Hooks->Release); Hooks->Ready->Trigger(); Hooks->Release->Wait(); }
 }
#endif
 Owner->AddRole(Bank,Role,bRootRole,-1); Owner=nullptr; Root.Reset(); // No Domain access after the last Root strong.
}
void FAllocationLifetime::StaticDestroyObject(const FAllocationLifetime* P)
{
 auto* Object=const_cast<FAllocationLifetime*>(P);
 if (!Object->Region.Domain()) { delete Object; return; }
 FRootRegionLease Keep=MoveTemp(Object->Region); FReservationDomain* D=Keep.Domain();
 Object->~FAllocationLifetime(); D->ConstructedSlotMask.fetch_and(uint16(~(1u<<uint8(EManualSlot::Lifetime))));
}
bool FAllocationAdapter::Calibrate(FReservationDomain& D,EControllerShape Shape,SIZE_T Request,uint32 Alignment)
{
 check(IsInGameThread()); auto& C=D.Identity.Certificates[uint8(Shape)];
 if (C.Attempted) return false;
 C.Request=Request; C.TypeAlignment=Alignment; C.OrdinaryAlignment=Request<=8?8:__STDCPP_DEFAULT_NEW_ALIGNMENT__; C.Support=State->Support; C.Attempted=true;
 State->CalibrationAttempted=true;
 if (Request==0 || Request>1024 || Alignment==0 || Alignment>16 || (Alignment&(Alignment-1)) || C.OrdinaryAlignment>16 || State->Support.Family==FSupportedAllocator::EFamily::Unsupported || !State->Support.bVerifiedModuleNew || !State->Support.bVerifiedNoInflatingProxy || State->Failed.load()) { C.Failed=true; return false; }
 if (!D.ReserveScratch()) { C.Failed=true; return false; }
 C.CertifiedBound=4096; D.Scratch.Request=Request; D.Scratch.Alignment=C.OrdinaryAlignment;
 LLM_SCOPE_BYTAG(VaRest_Inspector);
 void* P=FMemory::Malloc(Request,C.OrdinaryAlignment); C.Capacity=P?FMemory::GetAllocSize(P):0;
 D.Scratch.ObservedCapacity=int64(C.Capacity); D.Scratch.State=EClaimState::BackingLive;
 Probe->CalibrationRequest.store(int64(Request)); Probe->CalibrationAlignment.store(C.OrdinaryAlignment); Probe->CalibrationCapacity.store(int64(C.Capacity));
 ObservePeak(Probe->PeakBacking,Probe->LiveBacking.fetch_add(int64(C.Capacity))+int64(C.Capacity));
 FMemory::Free(P); Probe->LiveBacking.fetch_sub(int64(C.Capacity)); D.RetireScratch();
 C.Failed=!P || C.Capacity<Request || C.Capacity>4096;
 if (C.Failed) { State->Failed.store(true); AddSaturated(Probe->BoundFailures); }
 else State->Support.bCalibrationPassed=true;
 C.Support=State->Support; return !C.Failed;
}

FBothLedgerImage FReservationDomain::Export() const
{
 FBothLedgerImage I;
 I.Identity.DomainSerial=Identity.DomainSerial;
 I.Identity.PendingLedgerSerial=Identity.PendingLedgerSerial;
 I.Identity.StoreLedgerSerial=Identity.StoreLedgerSerial;
 I.Identity.MoveSerial=Identity.MoveSerial;
 I.Identity.NextClaimSerial=Identity.NextClaimSerial;
 I.Identity.NextBankSerial=Identity.NextBankSerial;
 I.Identity.StagingSerial=Identity.StagingSerial;
 I.Identity.NextGenerationSerial=Identity.NextGenerationSerial;
 I.Identity.LastBindingSerial=Identity.LastBindingSerial;
 I.Identity.ActiveBankSerial=Identity.ActiveBankSerial;
 I.Identity.Effective=Identity.Effective;
 I.Identity.Requested=Identity.Requested;
 I.Identity.CumulativeRefusals=Identity.CumulativeRefusals;
 I.Identity.ActiveBankSlot=Identity.ActiveBankSlot;
 I.Identity.PolicyBits=Identity.PolicyBits;
 I.Identity.StickyRefusal=Identity.StickyRefusal;
 I.Identity.RequestedPending=Identity.RequestedPending;
 I.Identity.Closed=Identity.Closed;
 I.Identity.Place=Identity.Place;
 I.Identity.AuthorityPhase=Identity.AuthorityPhase;
 I.Identity.DomainPhase=Identity.DomainPhase;
 I.Identity.AdmissionEpoch=AdmissionEpoch.load(); I.Identity.LossSaturated=LossSaturated.load();
 for (uint8 B=0;B<2;++B) static_cast<FBankIdentity&>(I.Identity.Banks[B])=Identity.Banks[B];
 for (uint8 N=0;N<uint8(EControllerShape::Count);++N) I.Identity.Certificates[N]=Identity.Certificates[N];
 I.GateWord=GateWord.load();
 I.PendingTotal=PendingTotal.load();
 I.PendingPeak=PendingPeak.load();
 I.StoreTotal=StoreTotal.load();
 I.StorePeak=StorePeak.load();
 I.SnapshotControllers=SnapshotControllers.load();
 I.ConstructedSlotMask=ConstructedSlotMask.load();
 I.PublicStringsUnsupported=PublicStringsUnsupported.load();
 I.Pending.Count=Pending.Count.load();
 I.Pending.Bytes=Pending.Bytes.load();
 I.Pending.PeakCount=Pending.PeakCount.load();
 I.Pending.PeakBytes=Pending.PeakBytes.load();
 I.Pending.ExcerptReservations=Pending.ExcerptReservations.load();
 I.Pending.DroppedCount=Pending.DroppedCount.load();
 I.Pending.DroppedBytes=Pending.DroppedBytes.load();
 I.Pending.MetadataOmitted=Pending.MetadataOmitted.load();
 I.Pending.ExcerptsSkipped=Pending.ExcerptsSkipped.load();
 I.Pending.LossEpoch=Pending.LossEpoch.load();
 I.ContextProbe.LiveBacking=ContextProbe.LiveBacking.load();
 I.ContextProbe.PeakBacking=ContextProbe.PeakBacking.load();
 I.ContextProbe.TextBytesCopied=ContextProbe.TextBytesCopied.load();
 I.ContextProbe.ExcerptRefsAcquired=ContextProbe.ExcerptRefsAcquired.load();
 I.ContextProbe.OverlapObservations=ContextProbe.OverlapObservations.load();
 I.ContextProbe.LiveControllers=ContextProbe.LiveControllers.load();
 I.ContextProbe.DestroyedControllers=ContextProbe.DestroyedControllers.load();
 I.ContextProbe.UnsupportedAllocator=ContextProbe.UnsupportedAllocator.load();
 I.ContextProbe.BoundFailures=ContextProbe.BoundFailures.load();
 I.ContextProbe.CalibrationRequest=ContextProbe.CalibrationRequest.load();
 I.ContextProbe.CalibrationAlignment=ContextProbe.CalibrationAlignment.load();
 I.ContextProbe.CalibrationCapacity=ContextProbe.CalibrationCapacity.load();
 I.StoreProbe.LiveBacking=StoreProbe.LiveBacking.load();
 I.StoreProbe.PeakBacking=StoreProbe.PeakBacking.load();
 I.StoreProbe.TextBytesCopied=StoreProbe.TextBytesCopied.load();
 I.StoreProbe.ExcerptRefsAcquired=StoreProbe.ExcerptRefsAcquired.load();
 I.StoreProbe.OverlapObservations=StoreProbe.OverlapObservations.load();
 I.StoreProbe.LiveControllers=StoreProbe.LiveControllers.load();
 I.StoreProbe.DestroyedControllers=StoreProbe.DestroyedControllers.load();
 I.StoreProbe.UnsupportedAllocator=StoreProbe.UnsupportedAllocator.load();
 I.StoreProbe.BoundFailures=StoreProbe.BoundFailures.load();
 I.StoreProbe.CalibrationRequest=StoreProbe.CalibrationRequest.load();
 I.StoreProbe.CalibrationAlignment=StoreProbe.CalibrationAlignment.load();
 I.StoreProbe.CalibrationCapacity=StoreProbe.CalibrationCapacity.load();
 I.BootstrapAdapter=CaptureAdapter(BootstrapAdapter); I.LifetimeAdapter=CaptureAdapter(LifetimeAdapter);
 for (uint8 B=0;B<2;++B) for (uint8 R=0;R<uint8(EBankRole::Count);++R) I.Identity.Banks[B].Roles[R]=uint32(BankRoles[B][R].load());
 for (uint8 R=0;R<uint8(ERootRole::Count);++R) I.RootRoles[R]=uint32(RootRoles[R].load());
 for (uint8 N=0;N<16;++N) I.Claims[N]=Claims[N]; I.Scratch=Scratch; I.ScratchLive=ScratchLive; return I;
}
bool FReservationDomain::Adopt(FReservationDomain& Source,ELedgerPlace Place)
{
 if (Identity.AuthorityPhase!=EAuthorityPhase::Dormant || Source.Identity.AuthorityPhase!=EAuthorityPhase::Active || Source.GateWord.load()!=ClosedBit || Source.Identity.MoveSerial==MAX_uint64 || !Source.RolesZero()) return false;
 const FBothLedgerImage I=Source.Export(); Source.Identity.AuthorityPhase=EAuthorityPhase::Frozen;
 Identity.DomainSerial=I.Identity.DomainSerial;
 Identity.PendingLedgerSerial=I.Identity.PendingLedgerSerial;
 Identity.StoreLedgerSerial=I.Identity.StoreLedgerSerial;
 Identity.MoveSerial=I.Identity.MoveSerial;
 Identity.NextClaimSerial=I.Identity.NextClaimSerial;
 Identity.NextBankSerial=I.Identity.NextBankSerial;
 Identity.StagingSerial=I.Identity.StagingSerial;
 Identity.NextGenerationSerial=I.Identity.NextGenerationSerial;
 Identity.LastBindingSerial=I.Identity.LastBindingSerial;
 Identity.ActiveBankSerial=I.Identity.ActiveBankSerial;
 Identity.Effective=I.Identity.Effective;
 Identity.Requested=I.Identity.Requested;
 Identity.CumulativeRefusals=I.Identity.CumulativeRefusals;
 Identity.ActiveBankSlot=I.Identity.ActiveBankSlot;
 Identity.PolicyBits=I.Identity.PolicyBits;
 Identity.StickyRefusal=I.Identity.StickyRefusal;
 Identity.RequestedPending=I.Identity.RequestedPending;
 Identity.Closed=I.Identity.Closed;
 Identity.Place=I.Identity.Place;
 Identity.AuthorityPhase=I.Identity.AuthorityPhase;
 Identity.DomainPhase=I.Identity.DomainPhase;
 Identity.AuthorityPhase=EAuthorityPhase::Dormant; AdmissionEpoch.store(I.Identity.AdmissionEpoch); LossSaturated.store(I.Identity.LossSaturated);
 for (uint8 B=0;B<2;++B) Identity.Banks[B]=static_cast<const FBankIdentity&>(I.Identity.Banks[B]);
 for (uint8 N=0;N<uint8(EControllerShape::Count);++N) Identity.Certificates[N]=I.Identity.Certificates[N];
 GateWord.store(I.GateWord);
 PendingTotal.store(I.PendingTotal);
 PendingPeak.store(I.PendingPeak);
 StoreTotal.store(I.StoreTotal);
 StorePeak.store(I.StorePeak);
 SnapshotControllers.store(I.SnapshotControllers);
 ConstructedSlotMask.store(I.ConstructedSlotMask);
 PublicStringsUnsupported.store(I.PublicStringsUnsupported);
 Pending.Count.store(I.Pending.Count);
 Pending.Bytes.store(I.Pending.Bytes);
 Pending.PeakCount.store(I.Pending.PeakCount);
 Pending.PeakBytes.store(I.Pending.PeakBytes);
 Pending.ExcerptReservations.store(I.Pending.ExcerptReservations);
 Pending.DroppedCount.store(I.Pending.DroppedCount);
 Pending.DroppedBytes.store(I.Pending.DroppedBytes);
 Pending.MetadataOmitted.store(I.Pending.MetadataOmitted);
 Pending.ExcerptsSkipped.store(I.Pending.ExcerptsSkipped);
 Pending.LossEpoch.store(I.Pending.LossEpoch);
 ContextProbe.LiveBacking.store(I.ContextProbe.LiveBacking);
 ContextProbe.PeakBacking.store(I.ContextProbe.PeakBacking);
 ContextProbe.TextBytesCopied.store(I.ContextProbe.TextBytesCopied);
 ContextProbe.ExcerptRefsAcquired.store(I.ContextProbe.ExcerptRefsAcquired);
 ContextProbe.OverlapObservations.store(I.ContextProbe.OverlapObservations);
 ContextProbe.LiveControllers.store(I.ContextProbe.LiveControllers);
 ContextProbe.DestroyedControllers.store(I.ContextProbe.DestroyedControllers);
 ContextProbe.UnsupportedAllocator.store(I.ContextProbe.UnsupportedAllocator);
 ContextProbe.BoundFailures.store(I.ContextProbe.BoundFailures);
 ContextProbe.CalibrationRequest.store(I.ContextProbe.CalibrationRequest);
 ContextProbe.CalibrationAlignment.store(I.ContextProbe.CalibrationAlignment);
 ContextProbe.CalibrationCapacity.store(I.ContextProbe.CalibrationCapacity);
 StoreProbe.LiveBacking.store(I.StoreProbe.LiveBacking);
 StoreProbe.PeakBacking.store(I.StoreProbe.PeakBacking);
 StoreProbe.TextBytesCopied.store(I.StoreProbe.TextBytesCopied);
 StoreProbe.ExcerptRefsAcquired.store(I.StoreProbe.ExcerptRefsAcquired);
 StoreProbe.OverlapObservations.store(I.StoreProbe.OverlapObservations);
 StoreProbe.LiveControllers.store(I.StoreProbe.LiveControllers);
 StoreProbe.DestroyedControllers.store(I.StoreProbe.DestroyedControllers);
 StoreProbe.UnsupportedAllocator.store(I.StoreProbe.UnsupportedAllocator);
 StoreProbe.BoundFailures.store(I.StoreProbe.BoundFailures);
 StoreProbe.CalibrationRequest.store(I.StoreProbe.CalibrationRequest);
 StoreProbe.CalibrationAlignment.store(I.StoreProbe.CalibrationAlignment);
 StoreProbe.CalibrationCapacity.store(I.StoreProbe.CalibrationCapacity);
 RestoreAdapter(BootstrapAdapter,I.BootstrapAdapter); RestoreAdapter(LifetimeAdapter,I.LifetimeAdapter);
 for (uint8 B=0;B<2;++B) for (uint8 R=0;R<uint8(EBankRole::Count);++R) BankRoles[B][R].store(int32(I.Identity.Banks[B].Roles[R]));
 for (uint8 R=0;R<uint8(ERootRole::Count);++R) RootRoles[R].store(int32(I.RootRoles[R]));
 for (uint8 N=0;N<16;++N) Claims[N]=I.Claims[N]; Scratch=I.Scratch; ScratchLive=I.ScratchLive;
 ++Identity.MoveSerial; Identity.Place=Place;
 FBothLedgerImage Expected=I; ++Expected.Identity.MoveSerial; Expected.Identity.Place=Place; Expected.Identity.AuthorityPhase=EAuthorityPhase::Dormant;
 if (!EqualAuthorityImages(Export(),Expected)) { Source.Identity.AuthorityPhase=EAuthorityPhase::Active; return false; }
 Source.Identity.AuthorityPhase=EAuthorityPhase::Moved; Identity.AuthorityPhase=EAuthorityPhase::Active; return true;
}
} // namespace VaRest::Inspector::Private
#endif
