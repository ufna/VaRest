// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestInspectorStore.h"
#include "VaRestInspectorQueue.h"
#include "Misc/CoreDelegates.h"
#include "Math/NumericLimits.h"
#include "Templates/SharedPointerInternals.h"

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
class FSink;
struct FCompactTerminalLedgerImage { std::byte Bytes[890]{}; };
enum class EReceiptPhase:uint8 { Dormant,Receiving,OwnsBacking,PayloadFreed,Closed };
struct FRootRetireReceipt
{
 std::atomic<EReceiptPhase> Phase{EReceiptPhase::Dormant};
 FCompactTerminalLedgerImage Image;
 bool Receive(FReservationDomain&);
 void BackingFreed(int64 Actual);
 void ControllerRetired();
 bool ReadFreed(FBothLedgerImage&) const;
};
struct FRootPublishSlot { FRootRetireReceipt* Receipt=nullptr; FReservationDomain* Domain=nullptr; };
class FRootStoredDeleter
{
public:
 explicit FRootStoredDeleter(FRootPublishSlot& P):bStored(false),Publish(&P) {}
 FRootStoredDeleter(FRootStoredDeleter&&) noexcept;
 FRootStoredDeleter(const FRootStoredDeleter&)=delete;
 ~FRootStoredDeleter();
 void operator()(FProductionRootBlock*);
private:
 bool bStored;
 union { FRootPublishSlot* Publish; FRootRetireReceipt Receipt; };
};

// Each exact controller has an independently calibrated type. Payload is a manual region.
template<class T> class TRegionStoredDeleter
{
public:
 TRegionStoredDeleter(FRootRegionLease&& L,EManualSlot S,uint8 C,int8 B,uint8 P,bool bR)
  : Lease(MoveTemp(L)),Slot(S),Claim(C),Bank(B),PayloadRole(P),bRootRole(bR) {}
 TRegionStoredDeleter(TRegionStoredDeleter&& O) noexcept
  : Lease(MoveTemp(O.Lease)),Slot(O.Slot),Claim(O.Claim),Bank(O.Bank),PayloadRole(O.PayloadRole),bRootRole(O.bRootRole),bStored(true)
 { check(!O.bStored); auto* D=Lease.Domain(); D->ObserveClaim(Claim,-1); D->ContextProbe.LiveControllers.fetch_add(1); }
 TRegionStoredDeleter(const TRegionStoredDeleter&)=delete;
 ~TRegionStoredDeleter()
 {
  if (bStored) { auto* D=Lease.Domain(); D->ContextProbe.LiveControllers.fetch_sub(1); D->ContextProbe.DestroyedControllers.fetch_add(1); D->RetireClaim(Claim); }
  // Lease declared FIRST, destroyed LAST, after the final weak actual stored deleter work.
 }
 void operator()(T* P)
 {
  auto* D=Lease.Domain(); P->~T();
  D->ConstructedSlotMask.fetch_and(uint16(~(1u<<uint8(Slot))));
  if (PayloadRole<(bRootRole?uint8(ERootRole::Count):uint8(EBankRole::Count))) D->AddRole(Bank,PayloadRole,bRootRole,-1);
 }
private:
 FRootRegionLease Lease;
 EManualSlot Slot; uint8 Claim; int8 Bank; uint8 PayloadRole; bool bRootRole,bStored=false;
};

template<class T> class TPendingOwnerDeleter
{
public:
 TPendingOwnerDeleter(TSharedRef<FPendingAccount,ESPMode::ThreadSafe> A,FPendingBaseline&& P,FPendingBaseline&& C,EBankRole PR,EBankRole CR)
  : Foreign(A->Lifetime->Region.PinRoot(),*A->Domain,A->Bank,uint8(EBankRole::ForeignAccount),false),Account(MoveTemp(A)),Payload(MoveTemp(P)),Controller(MoveTemp(C)),PayloadRole(PR),ControllerRole(CR) {}
 TPendingOwnerDeleter(TPendingOwnerDeleter&& O) noexcept
  : Foreign(MoveTemp(O.Foreign)),Account(MoveTemp(O.Account)),Payload(MoveTemp(O.Payload)),Controller(MoveTemp(O.Controller)),PayloadRole(O.PayloadRole),ControllerRole(O.ControllerRole),bStored(true)
 { check(!O.bStored); if (Account->Domain) Account->Domain->AddRole(Account->Bank,uint8(ControllerRole),false,1); Account->Lifetime->Probe.LiveControllers.fetch_add(1); }
 TPendingOwnerDeleter(const TPendingOwnerDeleter&)=delete;
 ~TPendingOwnerDeleter()
 {
  if (bStored) { Account->Lifetime->Probe.LiveControllers.fetch_sub(1); Account->Lifetime->Probe.DestroyedControllers.fetch_add(1); if (Account->Domain) Account->Domain->AddRole(Account->Bank,uint8(ControllerRole),false,-1); }
 }
 void operator()(T* P)
 {
  FAllocationAdapter& A=Account->Lifetime->Allocator; P->~T(); A.Free(P);
  if (Account->Domain) Account->Domain->AddRole(Account->Bank,uint8(PayloadRole),false,-1);
  Payload.Reset();
 }
private:
 // FIRST/last-destroyed: excludes bank reuse through every baseline and Account destructor.
 FRootRegionLease Foreign;
 TSharedRef<FPendingAccount,ESPMode::ThreadSafe> Account;
 FPendingBaseline Payload,Controller;
 EBankRole PayloadRole,ControllerRole; bool bStored=false;
};

struct FOverflowRecovery
{
 uint64 HandledLoss=0,HandledOrphan=0,TargetLoss=0,TargetOrphan=0,Watermark=0;
 double LastRun=-TNumericLimits<double>::Max(); bool bPending=false;
 void Observe(uint64 Loss,uint64 Orphan,uint64 Published);
 bool CanRun(double Now,uint64 Consumed) const;
 void Complete(double Now);
};
struct FCallbackArgument { TWeakPtr<class FService,ESPMode::ThreadSafe> Service; struct FCallbackState* State=nullptr; };
struct FCallbackState
{
 FReservationDomain* Domain=nullptr;
 FDelegateHandle EndFrame,PreExit;
 uint64 LastFrame=MAX_uint64,BindingSerial=0;
 int32 FrameDepth=0; bool Closed=true,Constructing=true;
 int64 FrameActual=0,ExitActual=0;
 // GT-only detachable facade route; never an owning callback or a worker use.
 bool* FacadeClosed=nullptr;
};
struct FTypeLayoutRow { const TCHAR* Name; SIZE_T Size; uint32 Alignment; SIZE_T Request; };
struct FProductionClockSelection { bool bDeterministic=false; };
#if WITH_DEV_AUTOMATION_TESTS
class FDeterministicReceiptClock final : public FMonotonicReceiptClock
{
public:
 double Now() const override { return double(Nanos.load())/1000000000.0; }
 void AdvanceNanos(int64 N) { check(N>=0); Nanos.fetch_add(N); }
private: std::atomic<int64> Nanos{0};
};
enum class EFactoryFailure:uint8 { None,BootstrapCalibration,RootBacking,RootController,Lifetime,PendingController,ClockController,ServiceController,FixedStorage,FrameBinding,PreExitBinding,Attach,Staging };
struct FServiceFactoryWitness
{
 TWeakPtr<FProductionRootBlock,ESPMode::ThreadSafe> Root;
 FRootRetireReceipt* Receipt=nullptr;
 FBothLedgerImage BootstrapFinal;
 bool bRootCreated=false,bBootstrapCaptured=false;
 void Reset() { Receipt=nullptr; Root.Reset(); bRootCreated=bBootstrapCaptured=false; }
 ~FServiceFactoryWitness() { Reset(); }
};
struct FServiceTestOptions { EFactoryFailure RefuseBefore=EFactoryFailure::None; bool bDeterministicClock=true; FServiceFactoryWitness* Witness=nullptr; };
struct FServiceTestAccess;
struct FServiceTestImage
{
 uint64 RecoveryCount=0,LastRecoveryWatermark=0,Generation=0,Revision=0;
 int64 PendingTotal=0,PendingDynamic=0,StoreTotal=0,PendingPeak=0,StorePeak=0,Controllers=0,Entries=0;
 int64 EffectiveStore=0,EffectivePending=0,RequestedStore=0,RequestedPending=0,SnapshotControllers=0,LiveBacking=0,TextCopies=0,ExcerptRefs=0;
 uint64 LossEpoch=0;
 bool bCapturing=false,bPendingLimits=false,bShutdown=false;
};
#endif
class FService
{
public:
 static bool TryCreate(const FCapturePolicy&,TSharedPtr<FService,ESPMode::ThreadSafe>& Out);
 static uint32 DescribeProductionTypes(FTypeLayoutRow*,uint32 Capacity);
 static bool InitialCapturePolicy(bool bEditor,bool bUnattended,bool bEditorSetting,bool bPackagedSetting,const TCHAR* CommandLine);
 ~FService();
 bool Start(const FCapturePolicy&);
 void RequestPolicy(const FCapturePolicy&);
 void Pause(); void Clear(); void Tick(); void Shutdown();
 void ApplyAttached(const FVaRestTraceSnapshot&);
 void SetCapture(bool);
 bool IsCapturing() const { return bCapturing && !bShutdown; }
 bool IsShutdown() const { return bShutdown; }
 uint64 Revision() const;
 bool TrySnapshot(TSharedPtr<const FVaRestInspectorSnapshot,ESPMode::ThreadSafe>&) const;
 bool ForEachRecord(TFunctionRef<void(const FVaRestInspectorRecordView&)>);
 void AttachFacade(FVaRestInspectorChanged&,bool* ShutdownFlag);
 void DetachFacade();
 enum class EControl:uint8 { None,Start,Pause,Clear,Shutdown };
 void RequestControl(EControl); void FlushDeferredControl();
#if WITH_DEV_AUTOMATION_TESTS
 static bool TryCreateForTest(const FCapturePolicy&,const FServiceTestOptions&,TSharedPtr<FService,ESPMode::ThreadSafe>&);
 FServiceTestImage TestImage() const;
 TSharedPtr<FCaptureQueue,ESPMode::ThreadSafe> QueueForTest() const { return ActiveQueue; }
 TSharedPtr<FSink,ESPMode::ThreadSafe> SinkForTest() const { return ActiveSink; }
 TSharedPtr<FDeterministicReceiptClock,ESPMode::ThreadSafe> ClockForTest() const;
 TWeakPtr<FProductionRootBlock,ESPMode::ThreadSafe> RootForTest() const;
 FRootRetireReceipt* ReceiptForTest() const { return Receipt; }
 void RequestLimitsForTest(const FNormalizedLimits&);
#endif
private:
#if WITH_DEV_AUTOMATION_TESTS
 friend struct FServiceTestAccess;
#endif
 friend class FSink;
 friend class FRootStoredDeleter;
 template<class> friend class TRegionStoredDeleter;
 FService(FReservationDomain&,FCallbackState&,const FCapturePolicy&,TRefCountPtr<FAllocationLifetime>,TRefCountPtr<FBudgetAccount>,TSharedRef<FPendingAccount,ESPMode::ThreadSafe>,TSharedRef<FMonotonicReceiptClock,ESPMode::ThreadSafe>);
 static bool Create(const FCapturePolicy&,FProductionClockSelection,uint8 Failure,TSharedPtr<FService,ESPMode::ThreadSafe>&
#if WITH_DEV_AUTOMATION_TESTS
 ,FServiceFactoryWitness* Witness=nullptr
#endif
 );
 static void EndFrame(FCallbackArgument); static void PreExit(FCallbackArgument);
 bool BindCallbacks(); void RemoveCallbacks();
 bool StartNow(const FCapturePolicy&); void PauseNow(); void ClearNow(); void ShutdownNow();
 void CloseGeneration(); bool RecreateAccounts(); void ApplyRequestedPolicy();
 void MarkChanged(); void Notify();
 template<class T,class... Args> TSharedPtr<T,ESPMode::ThreadSafe> MakePendingOwner(EBankRole,EBankRole,Args&&...);
 FReservationDomain* Domain;
 FCallbackState* CallbackState;
 FCapturePolicy Policy,RequestedPolicy;
 TRefCountPtr<FAllocationLifetime> Lifetime;
 TRefCountPtr<FBudgetAccount> Budget;
 TSharedPtr<FPendingAccount,ESPMode::ThreadSafe> Pending;
 TSharedRef<FMonotonicReceiptClock,ESPMode::ThreadSafe> Clock;
 FStore Store;
 TOptional<FBudgetControlLease> Control;
 TSharedPtr<FCaptureQueue,ESPMode::ThreadSafe> ActiveQueue;
 TSharedPtr<FSink,ESPMode::ThreadSafe> ActiveSink;
 TWeakPtr<FService,ESPMode::ThreadSafe> Self;
 FVaRestInspectorChanged* ChangedRoute=nullptr;
 FRootRetireReceipt* Receipt=nullptr;
 FOverflowRecovery Recovery;
 uint64 Generation=0,ServiceRevision=0,NotifiedRevision=0,RecoveryCount=0,LastRecoveryWatermark=0;
 int32 DispatchDepth=0;
 bool bCapturing=false,bShutdown=false,bDirty=false,bDeferredClear=false,bDeferredShutdown=false,bHasCaptureRequest=false,bRequestedCapture=false,bTicking=false,bHasPolicyRequest=false;
 uint8 RefuseBefore=0;
};

template<class T> struct TManualSlot
{
 alignas(T) std::byte Bytes[sizeof(T)];
 T* Get() { return reinterpret_cast<T*>(Bytes); }
};
union FClockManualChoice
{
 TManualSlot<FMonotonicReceiptClock> Default;
#if WITH_DEV_AUTOMATION_TESTS
 TManualSlot<FDeterministicReceiptClock> Deterministic;
#endif
 FClockManualChoice() {} ~FClockManualChoice() {}
};
struct FProductionRootBlock
{
 TManualSlot<FReservationDomain> Domain;
 TManualSlot<FAllocationLifetime> Lifetime;
 TManualSlot<FBudgetAccount> Budget[2];
 TManualSlot<FPendingAccount> Pending[2];
 TManualSlot<FService> Service;
 FClockManualChoice Clock;
 TManualSlot<FCallbackState> State;
};
}
#endif
