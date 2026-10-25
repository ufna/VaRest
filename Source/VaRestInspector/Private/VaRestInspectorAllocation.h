// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once
#include "HAL/Event.h"

#include "CoreTypes.h"
#include "HAL/LowLevelMemTracker.h"
#include "Misc/Optional.h"
#include "Templates/RefCounting.h"
#include "Templates/SharedPointer.h"
#include "VaRestInspector.h"
#include "VaRestInspectorPolicy.h"
#include <atomic>
#include <cstddef>
#include <new>

LLM_DECLARE_TAG(VaRest_Inspector);

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
bool TryReserve(std::atomic<int64>& Used, int64 Limit, int64 Amount, int64* OutUsed = nullptr);
bool AddChecked(int64& Sum, int64 Amount, int64 Limit);
void ObservePeak(std::atomic<int64>& Peak, int64 Value);
void AddSaturated(std::atomic<uint64>& Counter, uint64 Amount = 1);

struct FProductionRootBlock;
struct FReservationDomain;
struct FAllocationLifetime;

// Raw local storage is inactive in production; references bind the ONE Domain.
template<class T> class TLocalCells
{
public:
	explicit TLocalCells(bool bConstruct) : bLive(bConstruct) { if (bLive) new (Bytes) T(); }
	~TLocalCells() { if (bLive) Get().~T(); }
	TLocalCells(const TLocalCells&)=delete;
	TLocalCells& operator=(const TLocalCells&)=delete;
	T& Get() { return *reinterpret_cast<T*>(Bytes); }
private:
	alignas(T) std::byte Bytes[sizeof(T)];
	bool bLive;
};
struct FPendingCells
{
	std::atomic<int64> Count{0}, Bytes{0}, PeakCount{0}, PeakBytes{0}, ExcerptReservations{0};
	std::atomic<uint64> DroppedCount{0}, DroppedBytes{0}, MetadataOmitted{0}, ExcerptsSkipped{0}, LossEpoch{0};
};
enum class EBankRole : uint8 { BudgetPayload,PendingPayload,PendingController,StoreCredit,SnapshotController,ControlLease,PendingTicket,PendingBaseline,QueuePayload,QueueController,SinkPayload,SinkController,PreparedStorage,PreparedControl,ForeignAccount,Count };
enum class ERootRole : uint8 { ContextPayload,ServicePayload,ServiceController,ClockPayload,ClockController,CallbackFrame,DelegateArgument,StoreVisitor,ServiceFrame,ObserverFrame,StagingPermit,Count };
enum class EAuthorityPhase : uint8 { Dormant,Active,Frozen,Moved,Closed };
enum class ELedgerPlace : uint8 { Bootstrap,Domain,Receipt };
enum class EDomainPhase : uint8 { Unpublished,Live,Recreating,Retiring,Retired };
enum class EBankPhase : uint8 { Empty,Constructing,Active,Staging,Retiring,Reusable };
enum class EControllerShape : uint8 { Root,Pending,Service,Clock,DeterministicClock,Queue,Sink,Row,DispatchHeader,Snapshot,Count };
enum class EClaimKind : uint8 { RootBacking,RootController,ServiceController,ClockController,PendingController,FrameBinding,PreExitBinding,Scratch };
enum class EClaimState : uint8 { Empty,Reserved,BackingLive,Retired };
enum class ELedgers : uint8 { None=0,Pending=1,Store=2,Both=3 };
enum class EManualSlot : uint8 { Domain,Lifetime,BudgetA,BudgetB,PendingA,PendingB,Service,Clock,State };
struct FRootClaim
{
	uint64 Serial=0; int64 Reserved=0,ObservedCapacity=0; SIZE_T Request=0;
	uint32 Alignment=0; ELedgers Ledgers=ELedgers::None; EClaimKind Kind=EClaimKind::RootBacking; EClaimState State=EClaimState::Empty;
	bool IsEmpty() const;
};
struct FControllerCertificate
{
	SIZE_T Request=0,Capacity=0,CertifiedBound=0;
	uint32 TypeAlignment=0,OrdinaryAlignment=0;
	FSupportedAllocator Support; bool Attempted=false,Failed=false;
};
struct FAdapterImage { FSupportedAllocator Support; bool CalibrationAttempted=false,Failed=false; };
struct FAdapterState { FSupportedAllocator Support; bool CalibrationAttempted=false; std::atomic<bool> Failed{false}; };
struct FAllocationCounterImage
{
	int64 LiveBacking=0,PeakBacking=0,TextBytesCopied=0,ExcerptRefsAcquired=0,OverlapObservations=0,LiveControllers=0,DestroyedControllers=0;
	uint64 UnsupportedAllocator=0,BoundFailures=0;
	int64 CalibrationRequest=0,CalibrationAlignment=0,CalibrationCapacity=0;
};
struct FPendingImage
{
	int64 Count=0,Bytes=0,PeakCount=0,PeakBytes=0,ExcerptReservations=0;
	uint64 DroppedCount=0,DroppedBytes=0,MetadataOmitted=0,ExcerptsSkipped=0,LossEpoch=0;
};
struct FRefusalImage
{
	uint64 DroppedEvents=0,DroppedBytes=0,RefusedRows=0,MetadataOmitted=0,UnsupportedAllocator=0,InterruptedRows=0,AttachLosses=0,OmittedClients=0,ContractViolations=0,OmittedStatsGroups=0,OmittedLinks=0,OmittedMarkers=0,OmittedScopes=0,ExcerptsSkipped=0;
};
struct FBankIdentity
{
	uint64 Serial=0,BudgetIdentity=0;
	int64 CountLimit=0,ByteLimit=0,BudgetLimit=0,BootstrapBytes=0;
	EBankPhase Phase=EBankPhase::Empty;
};
struct FBankImage : FBankIdentity { uint32 Roles[uint8(EBankRole::Count)]{}; };
struct FDomainIdentity
{
	uint64 DomainSerial=0,PendingLedgerSerial=0,StoreLedgerSerial=0,MoveSerial=0,NextClaimSerial=0,NextBankSerial=0,AdmissionEpoch=0,StagingSerial=0,NextGenerationSerial=0,LastBindingSerial=0,ActiveBankSerial=0;
	FNormalizedLimits Effective,Requested;
	FControllerCertificate Certificates[uint8(EControllerShape::Count)];
	FBankImage Banks[2]; FRefusalImage CumulativeRefusals;
	uint8 ActiveBankSlot=0,PolicyBits=0;
	bool LossSaturated=false,StickyRefusal=false,RequestedPending=false,Closed=false;
	ELedgerPlace Place=ELedgerPlace::Bootstrap;
	EAuthorityPhase AuthorityPhase=EAuthorityPhase::Dormant;
	EDomainPhase DomainPhase=EDomainPhase::Unpublished;
};
struct FDomainMetadata
{
	uint64 DomainSerial=0,PendingLedgerSerial=0,StoreLedgerSerial=0,MoveSerial=0,NextClaimSerial=0,NextBankSerial=0,StagingSerial=0,NextGenerationSerial=0,LastBindingSerial=0,ActiveBankSerial=0;
	FNormalizedLimits Effective,Requested;
	FControllerCertificate Certificates[uint8(EControllerShape::Count)];
	FBankIdentity Banks[2]; FRefusalImage CumulativeRefusals;
	uint8 ActiveBankSlot=0,PolicyBits=0;
	bool StickyRefusal=false,RequestedPending=false,Closed=false;
	ELedgerPlace Place=ELedgerPlace::Bootstrap;
	EAuthorityPhase AuthorityPhase=EAuthorityPhase::Dormant;
	EDomainPhase DomainPhase=EDomainPhase::Unpublished;
};
struct FBothLedgerImage
{
	FDomainIdentity Identity;
	uint64 GateWord=0; int64 PendingTotal=0,PendingPeak=0,StoreTotal=0,StorePeak=0,SnapshotControllers=0;
	FPendingImage Pending; FAllocationCounterImage ContextProbe,StoreProbe;
	FAdapterImage BootstrapAdapter,LifetimeAdapter;
	uint32 RootRoles[uint8(ERootRole::Count)]{};
	uint16 ConstructedSlotMask=0; bool PublicStringsUnsupported=false;
	FRootClaim Claims[16],Scratch; bool ScratchLive=false;
};

// Move-only ownership of a physically embedded region; it never resides in Root as an automatic owner.
class FRootRegionLease
{
public:
	FRootRegionLease()=default;
	FRootRegionLease(TSharedPtr<FProductionRootBlock,ESPMode::ThreadSafe>,FReservationDomain&,int8 Bank,uint8 Role,bool bRootRole);
	FRootRegionLease(FRootRegionLease&&) noexcept;
	FRootRegionLease& operator=(FRootRegionLease&&) noexcept;
	FRootRegionLease(const FRootRegionLease&)=delete;
	~FRootRegionLease();
	FRootRegionLease Clone() const;
	void Reset();
	FReservationDomain* Domain() const { return Owner; }
	TSharedPtr<FProductionRootBlock,ESPMode::ThreadSafe> PinRoot() const { return Root; }
private:
	TSharedPtr<FProductionRootBlock,ESPMode::ThreadSafe> Root;
	FReservationDomain* Owner=nullptr; int8 Bank=-1; uint8 Role=0; bool bRootRole=false;
};

class FService;
class FStore;
class FRecreationStagingPermit
{
 friend class FService;
 friend class FStore;
 FRootRegionLease Guard;
 uint8 NextBank=0; uint64 Serial=0;
 FRecreationStagingPermit(FRootRegionLease&& G,uint8 B,uint64 S):Guard(MoveTemp(G)),NextBank(B),Serial(S) {}
 bool Valid(const FReservationDomain&,uint8 Bank) const;
public:
 FRecreationStagingPermit(const FRecreationStagingPermit&)=delete;
 FRecreationStagingPermit(FRecreationStagingPermit&&)=default;
};
class FRootQuiescenceProof
{
 friend class FService;
 friend class FStore;
 FRecreationStagingPermit* Permit;
 explicit FRootQuiescenceProof(FRecreationStagingPermit& P):Permit(&P) {}
public:
 FRootQuiescenceProof(const FRootQuiescenceProof&)=delete;
 FRootQuiescenceProof(FRootQuiescenceProof&&)=default;
};

struct FPendingAccount
{
private:
	TLocalCells<FPendingCells> Local;
public:
	FPendingAccount(int64 InCountLimit,int64 InByteLimit);
	FPendingAccount(FReservationDomain&,uint8 InBank,TRefCountPtr<FAllocationLifetime>);
	~FPendingAccount();
	FPendingAccount(const FPendingAccount&)=delete;
	const int64 CountLimit,ByteLimit;
	std::atomic<int64>& Count; std::atomic<int64>& Bytes; std::atomic<int64>& PeakCount; std::atomic<int64>& PeakBytes;
	std::atomic<int64>& ExcerptReservations;
	std::atomic<uint64>& DroppedCount; std::atomic<uint64>& DroppedBytes; std::atomic<uint64>& MetadataOmitted; std::atomic<uint64>& ExcerptsSkipped; std::atomic<uint64>& LossEpoch;
	TRefCountPtr<FAllocationLifetime> Lifetime;
	FReservationDomain* Domain=nullptr; uint8 Bank=0;
	bool ReserveBytes(int64 Charge,int64& OutBytes);
	void ReleaseBytes(int64 Charge);
};

class FPendingTicket
{
public:
	FPendingTicket() = default;
	FPendingTicket(const FPendingTicket&) = delete;
	FPendingTicket& operator=(const FPendingTicket&) = delete;
	FPendingTicket(FPendingTicket&& Other) noexcept;
	FPendingTicket& operator=(FPendingTicket&& Other) noexcept;
	~FPendingTicket();
	static TOptional<FPendingTicket> Acquire(const TSharedRef<FPendingAccount, ESPMode::ThreadSafe>& InAccount, int64 InCharge, bool* OutCountRefused = nullptr);
	void Reset();
	bool IsValid() const { return Account.IsValid(); }
	int64 Bytes() const { return Charge; }

private:
	TSharedPtr<FPendingAccount, ESPMode::ThreadSafe> Account;
	int64 Charge = 0;
};

class FPendingBaseline
{
public:
	FPendingBaseline() = default;
	FPendingBaseline(const FPendingBaseline&) = delete;
	FPendingBaseline& operator=(const FPendingBaseline&) = delete;
	FPendingBaseline(FPendingBaseline&& Other) noexcept;
	FPendingBaseline& operator=(FPendingBaseline&& Other) noexcept;
	~FPendingBaseline();
	static TOptional<FPendingBaseline> Acquire(const TSharedRef<FPendingAccount, ESPMode::ThreadSafe>& InAccount, int64 InCharge);
	void Reset();

private:
	TSharedPtr<FPendingAccount, ESPMode::ThreadSafe> Account;
	int64 Charge = 0;
};

struct FAllocationCounters
{
	std::atomic<int64> LiveBacking{0}, PeakBacking{0}, TextBytesCopied{0};
	std::atomic<int64> ExcerptRefsAcquired{0}, OverlapObservations{0};
	std::atomic<int64> LiveControllers{0}, DestroyedControllers{0};
	std::atomic<uint64> UnsupportedAllocator{0}, BoundFailures{0};
	std::atomic<int64> CalibrationRequest{0}, CalibrationAlignment{0}, CalibrationCapacity{0};
};

class FAllocationAdapter
{
public:
	FAllocationAdapter(FAllocationCounters& InCounters,FSupportedAllocator InSupport);
	FAllocationAdapter(FAllocationCounters&,FAdapterState&,FSupportedAllocator);
	FAllocationAdapter(const FAllocationAdapter&)=delete;
	FAllocationAdapter& operator=(const FAllocationAdapter&)=delete;
	bool Calibrate(std::atomic<int64>& StartupUsed,int64 StartupLimit,SIZE_T ControllerRequest,uint32 TypeAlignment);
	bool Calibrate(FReservationDomain&,EControllerShape,SIZE_T ControllerRequest,uint32 TypeAlignment);
	FSupportedAllocator GetSupport() const;
	FAdapterImage ExportState() const;
	TOptional<SIZE_T> Bound(SIZE_T Request,uint32 Alignment) const;
	void* AllocateReserved(SIZE_T Request,uint32 Alignment,SIZE_T ReservedBound);
	void Free(void* Ptr);
	FAllocationCounters& Counters() const;
	SIZE_T QueueNodeBound() const;
private:
	TLocalCells<FAdapterState> Local;
	FAllocationCounters* Probe;
	FAdapterState* State;
};

#if WITH_DEV_AUTOMATION_TESTS
// Borrowed barriers only. The fixture owns both events through worker join.
struct FAccountTeardownTestHooks
{
 FEvent* Ready=nullptr; FEvent* Release=nullptr;
 uint8 Bank=0; std::atomic<bool> Armed{false};
};
#endif
struct FReservationDomain
{
	static constexpr uint64 ClosedBit=uint64(1)<<63;
	FDomainMetadata Identity;
	std::atomic<uint64> AdmissionEpoch{0};
	std::atomic<bool> LossSaturated{false};
	std::atomic<uint64> GateWord{ClosedBit};
	std::atomic<int64> PendingTotal{0},PendingPeak{0},StoreTotal{0},StorePeak{0},SnapshotControllers{0};
	FPendingCells Pending;
	FAllocationCounters ContextProbe,StoreProbe;
	FAdapterState BootstrapAdapter,LifetimeAdapter;
	std::atomic<int32> BankRoles[2][uint8(EBankRole::Count)]{};
#if WITH_DEV_AUTOMATION_TESTS
 std::atomic<FAccountTeardownTestHooks*> TeardownTestHooks{nullptr};
#endif
	std::atomic<int32> RootRoles[uint8(ERootRole::Count)]{};
	std::atomic<uint16> ConstructedSlotMask{0};
	std::atomic<bool> PublicStringsUnsupported{false};
	FRootClaim Claims[16],Scratch; bool ScratchLive=false;
	explicit FReservationDomain(const FCapturePolicy&,FSupportedAllocator);
	FReservationDomain();
	bool Reserve(ELedgers,int64);
	void Release(ELedgers,int64);
	int64 PendingCap() const; int64 StoreCap() const;
	bool Enter(uint8 Bank); void Leave(); void Close(); bool Reopen(uint8 Bank);
	bool ReserveClaim(uint8 Slot,EClaimKind,SIZE_T,uint32,int64,ELedgers);
	void ObserveClaim(uint8 Slot,int64 Actual);
	void RetireClaim(uint8 Slot);
	bool ReserveScratch(); void RetireScratch();
	bool RolesZero() const; bool BankReusable(uint8) const;
	void AddRole(int8 Bank,uint8 Role,bool bRootRole,int32 Delta);
	FBothLedgerImage Export() const;
	bool Adopt(FReservationDomain& Source,ELedgerPlace NewPlace);
};
class FAdmissionEntry
{
public:
	explicit FAdmissionEntry(FPendingAccount& A) : Domain(A.Domain),Bank(A.Bank),bEntered(!Domain || Domain->Enter(A.Bank)) { if (Domain && bEntered) Epoch=Domain->AdmissionEpoch.load(std::memory_order_acquire); }
	~FAdmissionEntry() { if (Domain && bEntered) Domain->Leave(); }
	explicit operator bool() const { return bEntered; }
 bool StillCurrent() const { return bEntered && (!Domain || (Epoch==Domain->AdmissionEpoch.load(std::memory_order_acquire) && Domain->Identity.ActiveBankSlot==Bank && !(Domain->GateWord.load(std::memory_order_acquire)&FReservationDomain::ClosedBit))); }
private: FReservationDomain* Domain; uint64 Epoch=0; uint8 Bank; bool bEntered;
};

struct FAllocationLifetime final : TRefCountingMixin<FAllocationLifetime>
{
private:
	TLocalCells<FAllocationCounters> LocalProbe;
public:
	FAllocationCounters& Probe;
	FAllocationAdapter Allocator;
	FRootRegionLease Region;
	explicit FAllocationLifetime(FSupportedAllocator InSupport) : LocalProbe(true),Probe(LocalProbe.Get()),Allocator(Probe,InSupport) {}
	FAllocationLifetime(FReservationDomain& D,FSupportedAllocator S,FRootRegionLease&& InRegion)
		: LocalProbe(false),Probe(D.ContextProbe),Allocator(Probe,D.LifetimeAdapter,S),Region(MoveTemp(InRegion)) {}
	static void StaticDestroyObject(const FAllocationLifetime*);
};

bool EqualAuthorityImages(const FBothLedgerImage&,const FBothLedgerImage&);

// Pure current-process predicates; meaningful-off inspection does not create a context or calibrate.
FSupportedAllocator InspectCurrentAllocatorForFactory();
} // namespace VaRest::Inspector::Private
#endif
