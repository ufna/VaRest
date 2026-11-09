// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestInspectorService.h"
#include "VaRestInspectorSink.h"
#include "CoreGlobals.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Delegates/DelegateInstancesImpl.h"
#include <type_traits>
#include <cstdlib>

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
namespace
{
static_assert(sizeof(FCompactTerminalLedgerImage)==890 && alignof(FCompactTerminalLedgerImage)==1);
static_assert(std::extent_v<decltype(FBothLedgerImage::Claims)> ==16);
struct FByteWriter
{
 std::byte* Bytes; SIZE_T Offset=0; bool Valid=true;
 template<class T> void Put(T V) { if (Offset+sizeof(T)>890) { Valid=false; return; } FMemory::Memcpy(Bytes+Offset,&V,sizeof(T)); Offset+=sizeof(T); }
};
struct FByteReader
{
 const std::byte* Bytes; SIZE_T Offset=0; bool Valid=true;
 template<class T> T Get() { T V{}; if (Offset+sizeof(T)>890) { Valid=false; return V; } FMemory::Memcpy(&V,Bytes+Offset,sizeof(T)); Offset+=sizeof(T); return V; }
};
bool SameSupport(const FSupportedAllocator& A,const FSupportedAllocator& B)
{ return A.Family==B.Family && A.bVerifiedModuleNew==B.bVerifiedModuleNew && A.bVerifiedNoInflatingProxy==B.bVerifiedNoInflatingProxy && A.bCalibrationPassed==B.bCalibrationPassed; }
uint8 PackAdapter(const FAdapterImage& A)
{ return uint8(A.Support.Family)|(uint8(A.Support.bVerifiedModuleNew)<<2)|(uint8(A.Support.bVerifiedNoInflatingProxy)<<3)|(uint8(A.Support.bCalibrationPassed)<<4)|(uint8(A.CalibrationAttempted)<<5)|(uint8(A.Failed)<<6); }
FAdapterImage UnpackAdapter(uint8 V)
{
 FAdapterImage A; A.Support.Family=FSupportedAllocator::EFamily(V&3); A.Support.bVerifiedModuleNew=(V&4)!=0; A.Support.bVerifiedNoInflatingProxy=(V&8)!=0; A.Support.bCalibrationPassed=(V&16)!=0; A.CalibrationAttempted=(V&32)!=0; A.Failed=(V&64)!=0; return A;
}
bool SameClaim(const FRootClaim& A,const FRootClaim& B)
{ return A.Serial==B.Serial && A.Reserved==B.Reserved && A.ObservedCapacity==B.ObservedCapacity && A.Request==B.Request && A.Alignment==B.Alignment && A.Ledgers==B.Ledgers && A.Kind==B.Kind && A.State==B.State; }
bool SameImage(const FBothLedgerImage& A,const FBothLedgerImage& B)
{
 if (!(A.Identity.DomainSerial==B.Identity.DomainSerial &&
 A.Identity.PendingLedgerSerial==B.Identity.PendingLedgerSerial &&
 A.Identity.StoreLedgerSerial==B.Identity.StoreLedgerSerial &&
 A.Identity.MoveSerial==B.Identity.MoveSerial &&
 A.Identity.NextClaimSerial==B.Identity.NextClaimSerial &&
 A.Identity.NextBankSerial==B.Identity.NextBankSerial &&
 A.Identity.AdmissionEpoch==B.Identity.AdmissionEpoch &&
 A.Identity.StagingSerial==B.Identity.StagingSerial &&
 A.Identity.NextGenerationSerial==B.Identity.NextGenerationSerial &&
 A.Identity.LastBindingSerial==B.Identity.LastBindingSerial &&
 A.Identity.ActiveBankSerial==B.Identity.ActiveBankSerial &&
 A.Identity.ActiveBankSlot==B.Identity.ActiveBankSlot &&
 A.Identity.PolicyBits==B.Identity.PolicyBits &&
 A.Identity.LossSaturated==B.Identity.LossSaturated &&
 A.Identity.StickyRefusal==B.Identity.StickyRefusal &&
 A.Identity.RequestedPending==B.Identity.RequestedPending &&
 A.Identity.Closed==B.Identity.Closed &&
 A.Identity.Place==B.Identity.Place &&
 A.Identity.AuthorityPhase==B.Identity.AuthorityPhase &&
 A.Identity.DomainPhase==B.Identity.DomainPhase &&
 A.GateWord==B.GateWord &&
 A.PendingTotal==B.PendingTotal &&
 A.PendingPeak==B.PendingPeak &&
 A.StoreTotal==B.StoreTotal &&
 A.StorePeak==B.StorePeak &&
 A.SnapshotControllers==B.SnapshotControllers &&
 A.ConstructedSlotMask==B.ConstructedSlotMask &&
 A.PublicStringsUnsupported==B.PublicStringsUnsupported &&
 A.ScratchLive==B.ScratchLive &&
 A.Identity.Effective.StoreBytes==B.Identity.Effective.StoreBytes &&
 A.Identity.Effective.MaxRecords==B.Identity.Effective.MaxRecords &&
 A.Identity.Effective.BodyBytes==B.Identity.Effective.BodyBytes &&
 A.Identity.Effective.BinaryBytes==B.Identity.Effective.BinaryBytes &&
 A.Identity.Effective.FrameBytes==B.Identity.Effective.FrameBytes &&
 A.Identity.Effective.MaxFramesPerSocket==B.Identity.Effective.MaxFramesPerSocket &&
 A.Identity.Effective.PendingEvents==B.Identity.Effective.PendingEvents &&
 A.Identity.Effective.PendingBytes==B.Identity.Effective.PendingBytes &&
 A.Identity.Requested.StoreBytes==B.Identity.Requested.StoreBytes &&
 A.Identity.Requested.MaxRecords==B.Identity.Requested.MaxRecords &&
 A.Identity.Requested.BodyBytes==B.Identity.Requested.BodyBytes &&
 A.Identity.Requested.BinaryBytes==B.Identity.Requested.BinaryBytes &&
 A.Identity.Requested.FrameBytes==B.Identity.Requested.FrameBytes &&
 A.Identity.Requested.MaxFramesPerSocket==B.Identity.Requested.MaxFramesPerSocket &&
 A.Identity.Requested.PendingEvents==B.Identity.Requested.PendingEvents &&
 A.Identity.Requested.PendingBytes==B.Identity.Requested.PendingBytes &&
 A.Pending.Count==B.Pending.Count &&
 A.Pending.Bytes==B.Pending.Bytes &&
 A.Pending.PeakCount==B.Pending.PeakCount &&
 A.Pending.PeakBytes==B.Pending.PeakBytes &&
 A.Pending.ExcerptReservations==B.Pending.ExcerptReservations &&
 A.Pending.DroppedCount==B.Pending.DroppedCount &&
 A.Pending.DroppedBytes==B.Pending.DroppedBytes &&
 A.Pending.MetadataOmitted==B.Pending.MetadataOmitted &&
 A.Pending.ExcerptsSkipped==B.Pending.ExcerptsSkipped &&
 A.Pending.LossEpoch==B.Pending.LossEpoch &&
 A.ContextProbe.LiveBacking==B.ContextProbe.LiveBacking &&
 A.ContextProbe.PeakBacking==B.ContextProbe.PeakBacking &&
 A.ContextProbe.TextBytesCopied==B.ContextProbe.TextBytesCopied &&
 A.ContextProbe.ExcerptRefsAcquired==B.ContextProbe.ExcerptRefsAcquired &&
 A.ContextProbe.OverlapObservations==B.ContextProbe.OverlapObservations &&
 A.ContextProbe.LiveControllers==B.ContextProbe.LiveControllers &&
 A.ContextProbe.DestroyedControllers==B.ContextProbe.DestroyedControllers &&
 A.ContextProbe.UnsupportedAllocator==B.ContextProbe.UnsupportedAllocator &&
 A.ContextProbe.BoundFailures==B.ContextProbe.BoundFailures &&
 A.ContextProbe.CalibrationRequest==B.ContextProbe.CalibrationRequest &&
 A.ContextProbe.CalibrationAlignment==B.ContextProbe.CalibrationAlignment &&
 A.ContextProbe.CalibrationCapacity==B.ContextProbe.CalibrationCapacity &&
 A.StoreProbe.LiveBacking==B.StoreProbe.LiveBacking &&
 A.StoreProbe.PeakBacking==B.StoreProbe.PeakBacking &&
 A.StoreProbe.TextBytesCopied==B.StoreProbe.TextBytesCopied &&
 A.StoreProbe.ExcerptRefsAcquired==B.StoreProbe.ExcerptRefsAcquired &&
 A.StoreProbe.OverlapObservations==B.StoreProbe.OverlapObservations &&
 A.StoreProbe.LiveControllers==B.StoreProbe.LiveControllers &&
 A.StoreProbe.DestroyedControllers==B.StoreProbe.DestroyedControllers &&
 A.StoreProbe.UnsupportedAllocator==B.StoreProbe.UnsupportedAllocator &&
 A.StoreProbe.BoundFailures==B.StoreProbe.BoundFailures &&
 A.StoreProbe.CalibrationRequest==B.StoreProbe.CalibrationRequest &&
 A.StoreProbe.CalibrationAlignment==B.StoreProbe.CalibrationAlignment &&
 A.StoreProbe.CalibrationCapacity==B.StoreProbe.CalibrationCapacity &&
 A.Identity.CumulativeRefusals.DroppedEvents==B.Identity.CumulativeRefusals.DroppedEvents &&
 A.Identity.CumulativeRefusals.DroppedBytes==B.Identity.CumulativeRefusals.DroppedBytes &&
 A.Identity.CumulativeRefusals.RefusedRows==B.Identity.CumulativeRefusals.RefusedRows &&
 A.Identity.CumulativeRefusals.MetadataOmitted==B.Identity.CumulativeRefusals.MetadataOmitted &&
 A.Identity.CumulativeRefusals.UnsupportedAllocator==B.Identity.CumulativeRefusals.UnsupportedAllocator &&
 A.Identity.CumulativeRefusals.InterruptedRows==B.Identity.CumulativeRefusals.InterruptedRows &&
 A.Identity.CumulativeRefusals.AttachLosses==B.Identity.CumulativeRefusals.AttachLosses &&
 A.Identity.CumulativeRefusals.OmittedClients==B.Identity.CumulativeRefusals.OmittedClients &&
 A.Identity.CumulativeRefusals.ContractViolations==B.Identity.CumulativeRefusals.ContractViolations &&
 A.Identity.CumulativeRefusals.OmittedStatsGroups==B.Identity.CumulativeRefusals.OmittedStatsGroups &&
 A.Identity.CumulativeRefusals.OmittedLinks==B.Identity.CumulativeRefusals.OmittedLinks &&
 A.Identity.CumulativeRefusals.OmittedMarkers==B.Identity.CumulativeRefusals.OmittedMarkers &&
 A.Identity.CumulativeRefusals.OmittedScopes==B.Identity.CumulativeRefusals.OmittedScopes &&
 A.Identity.CumulativeRefusals.ExcerptsSkipped==B.Identity.CumulativeRefusals.ExcerptsSkipped)) return false;

 for (uint8 N=0;N<2;++N)
 {
  const auto& X=A.Identity.Banks[N]; const auto& Y=B.Identity.Banks[N];
  if (X.Serial!=Y.Serial || X.BudgetIdentity!=Y.BudgetIdentity || X.CountLimit!=Y.CountLimit || X.ByteLimit!=Y.ByteLimit || X.BudgetLimit!=Y.BudgetLimit || X.BootstrapBytes!=Y.BootstrapBytes || X.Phase!=Y.Phase) return false;
  for (uint8 R=0;R<uint8(EBankRole::Count);++R) if (X.Roles[R]!=Y.Roles[R]) return false;
 }
 for (uint8 N=0;N<uint8(EControllerShape::Count);++N)
 {
  const auto& X=A.Identity.Certificates[N]; const auto& Y=B.Identity.Certificates[N];
  if (X.Request!=Y.Request || X.Capacity!=Y.Capacity || X.CertifiedBound!=Y.CertifiedBound || X.TypeAlignment!=Y.TypeAlignment || X.OrdinaryAlignment!=Y.OrdinaryAlignment || !SameSupport(X.Support,Y.Support) || X.Attempted!=Y.Attempted || X.Failed!=Y.Failed) return false;
 }
 if (!SameSupport(A.BootstrapAdapter.Support,B.BootstrapAdapter.Support) || A.BootstrapAdapter.CalibrationAttempted!=B.BootstrapAdapter.CalibrationAttempted || A.BootstrapAdapter.Failed!=B.BootstrapAdapter.Failed || !SameSupport(A.LifetimeAdapter.Support,B.LifetimeAdapter.Support) || A.LifetimeAdapter.CalibrationAttempted!=B.LifetimeAdapter.CalibrationAttempted || A.LifetimeAdapter.Failed!=B.LifetimeAdapter.Failed) return false;
 for (uint8 R=0;R<uint8(ERootRole::Count);++R) if (A.RootRoles[R]!=B.RootRoles[R]) return false;
 for (uint8 N=0;N<16;++N) if (!SameClaim(A.Claims[N],B.Claims[N])) return false;
 return SameClaim(A.Scratch,B.Scratch);
}
bool Encode(const FBothLedgerImage& I,FCompactTerminalLedgerImage& Out)
{
 if (I.GateWord!=FReservationDomain::ClosedBit || I.ScratchLive || !I.Scratch.IsEmpty()) return false;
 for (const auto& B:I.Identity.Banks) for (uint32 R:B.Roles) if (R) return false;
 for (uint32 R:I.RootRoles) if (R) return false;
 for (uint8 N=2;N<16;++N) if (!I.Claims[N].IsEmpty()) return false;
 if (I.Identity.ActiveBankSlot>1 || I.Identity.PolicyBits>3 || uint8(I.Identity.Place)>2 || uint8(I.Identity.AuthorityPhase)>4 || uint8(I.Identity.DomainPhase)>4 || uint8(I.BootstrapAdapter.Support.Family)>2 || uint8(I.LifetimeAdapter.Support.Family)>2) return false;
 FByteWriter W{Out.Bytes}; W.Put<uint8>(1); W.Put<uint8>(0x0f);
 W.Put<uint64>(I.Identity.DomainSerial);
 W.Put<uint64>(I.Identity.PendingLedgerSerial);
 W.Put<uint64>(I.Identity.StoreLedgerSerial);
 W.Put<uint64>(I.Identity.MoveSerial);
 W.Put<uint64>(I.Identity.NextClaimSerial);
 W.Put<uint64>(I.Identity.NextBankSerial);
 W.Put<uint64>(I.Identity.AdmissionEpoch);
 W.Put<uint64>(I.Identity.StagingSerial);
 W.Put<uint64>(I.Identity.NextGenerationSerial);
 W.Put<uint64>(I.Identity.LastBindingSerial);
 W.Put<uint64>(I.Identity.ActiveBankSerial);
 W.Put<int64>(I.Identity.Effective.StoreBytes);
 W.Put<int32>(I.Identity.Effective.MaxRecords);
 W.Put<int32>(I.Identity.Effective.BodyBytes);
 W.Put<int32>(I.Identity.Effective.BinaryBytes);
 W.Put<int32>(I.Identity.Effective.FrameBytes);
 W.Put<int32>(I.Identity.Effective.MaxFramesPerSocket);
 W.Put<int32>(I.Identity.Effective.PendingEvents);
 W.Put<int64>(I.Identity.Effective.PendingBytes);
 W.Put<int64>(I.Identity.Requested.StoreBytes);
 W.Put<int32>(I.Identity.Requested.MaxRecords);
 W.Put<int32>(I.Identity.Requested.BodyBytes);
 W.Put<int32>(I.Identity.Requested.BinaryBytes);
 W.Put<int32>(I.Identity.Requested.FrameBytes);
 W.Put<int32>(I.Identity.Requested.MaxFramesPerSocket);
 W.Put<int32>(I.Identity.Requested.PendingEvents);
 W.Put<int64>(I.Identity.Requested.PendingBytes);
 W.Put<int64>(I.PendingTotal);
 W.Put<int64>(I.PendingPeak);
 W.Put<int64>(I.StoreTotal);
 W.Put<int64>(I.StorePeak);
 W.Put<int64>(I.SnapshotControllers);
 W.Put<int64>(I.Pending.Count);
 W.Put<int64>(I.Pending.Bytes);
 W.Put<int64>(I.Pending.PeakCount);
 W.Put<int64>(I.Pending.PeakBytes);
 W.Put<int64>(I.Pending.ExcerptReservations);
 W.Put<uint64>(I.Pending.DroppedCount);
 W.Put<uint64>(I.Pending.DroppedBytes);
 W.Put<uint64>(I.Pending.MetadataOmitted);
 W.Put<uint64>(I.Pending.ExcerptsSkipped);
 W.Put<uint64>(I.Pending.LossEpoch);
 W.Put<int64>(I.ContextProbe.LiveBacking);
 W.Put<int64>(I.ContextProbe.PeakBacking);
 W.Put<int64>(I.ContextProbe.TextBytesCopied);
 W.Put<int64>(I.ContextProbe.ExcerptRefsAcquired);
 W.Put<int64>(I.ContextProbe.OverlapObservations);
 W.Put<int64>(I.ContextProbe.LiveControllers);
 W.Put<int64>(I.ContextProbe.DestroyedControllers);
 W.Put<uint64>(I.ContextProbe.UnsupportedAllocator);
 W.Put<uint64>(I.ContextProbe.BoundFailures);
 W.Put<int64>(I.ContextProbe.CalibrationRequest);
 W.Put<int64>(I.ContextProbe.CalibrationAlignment);
 W.Put<int64>(I.ContextProbe.CalibrationCapacity);
 W.Put<int64>(I.StoreProbe.LiveBacking);
 W.Put<int64>(I.StoreProbe.PeakBacking);
 W.Put<int64>(I.StoreProbe.TextBytesCopied);
 W.Put<int64>(I.StoreProbe.ExcerptRefsAcquired);
 W.Put<int64>(I.StoreProbe.OverlapObservations);
 W.Put<int64>(I.StoreProbe.LiveControllers);
 W.Put<int64>(I.StoreProbe.DestroyedControllers);
 W.Put<uint64>(I.StoreProbe.UnsupportedAllocator);
 W.Put<uint64>(I.StoreProbe.BoundFailures);
 W.Put<int64>(I.StoreProbe.CalibrationRequest);
 W.Put<int64>(I.StoreProbe.CalibrationAlignment);
 W.Put<int64>(I.StoreProbe.CalibrationCapacity);

 W.Put<uint8>(PackAdapter(I.BootstrapAdapter)); W.Put<uint8>(PackAdapter(I.LifetimeAdapter));
 for (const auto& C:I.Identity.Certificates)
 {
  const auto Alignment=[](uint32 A) { return A==0 || (A<=16 && (A&(A-1))==0); };
  if (C.Request>1024 || !Alignment(C.TypeAlignment) || !Alignment(C.OrdinaryAlignment) || (C.CertifiedBound!=0 && C.CertifiedBound!=4096) || uint8(C.Support.Family)>2) return false;
  W.Put<uint64>(uint64(C.Capacity)); W.Put<uint16>(uint16(C.Request)); W.Put<uint8>(uint8(C.TypeAlignment)); W.Put<uint8>(uint8(C.OrdinaryAlignment));
  W.Put<uint8>(PackAdapter({C.Support,C.Attempted,C.Failed}) | (C.CertifiedBound?128:0));
 }
 for (const auto& B:I.Identity.Banks)
 {
  if (uint8(B.Phase)>5) return false;
  W.Put<uint64>(B.Serial); W.Put<uint64>(B.BudgetIdentity); W.Put<int64>(B.CountLimit); W.Put<int64>(B.ByteLimit); W.Put<int64>(B.BudgetLimit); W.Put<int64>(B.BootstrapBytes); W.Put<uint8>(uint8(B.Phase));
 }
 W.Put<uint64>(I.Identity.CumulativeRefusals.DroppedEvents);
 W.Put<uint64>(I.Identity.CumulativeRefusals.DroppedBytes);
 W.Put<uint64>(I.Identity.CumulativeRefusals.RefusedRows);
 W.Put<uint64>(I.Identity.CumulativeRefusals.MetadataOmitted);
 W.Put<uint64>(I.Identity.CumulativeRefusals.UnsupportedAllocator);
 W.Put<uint64>(I.Identity.CumulativeRefusals.InterruptedRows);
 W.Put<uint64>(I.Identity.CumulativeRefusals.AttachLosses);
 W.Put<uint64>(I.Identity.CumulativeRefusals.OmittedClients);
 W.Put<uint64>(I.Identity.CumulativeRefusals.ContractViolations);
 W.Put<uint64>(I.Identity.CumulativeRefusals.OmittedStatsGroups);
 W.Put<uint64>(I.Identity.CumulativeRefusals.OmittedLinks);
 W.Put<uint64>(I.Identity.CumulativeRefusals.OmittedMarkers);
 W.Put<uint64>(I.Identity.CumulativeRefusals.OmittedScopes);
 W.Put<uint64>(I.Identity.CumulativeRefusals.ExcerptsSkipped);

 W.Put<uint16>(I.ConstructedSlotMask); W.Put<uint8>(I.Identity.ActiveBankSlot); W.Put<uint8>(I.Identity.PolicyBits);
 W.Put<uint8>(uint8(I.PublicStringsUnsupported)|(uint8(I.Identity.LossSaturated)<<1)|(uint8(I.Identity.StickyRefusal)<<2)|(uint8(I.Identity.RequestedPending)<<3)|(uint8(I.Identity.Closed)<<4));
 W.Put<uint8>(uint8(I.Identity.Place)|(uint8(I.Identity.AuthorityPhase)<<2)|(uint8(I.Identity.DomainPhase)<<5));
 for (uint8 N=0;N<2;++N)
 {
  const auto& C=I.Claims[N];
  if (C.Request>SIZE_T(FNormalizedLimits::MaxMetadataBytes) || C.Alignment>16 || (C.Alignment && (C.Alignment&(C.Alignment-1))) || uint8(C.Ledgers)>3 || uint8(C.Kind)>7 || uint8(C.State)>3 || (C.Ledgers==ELedgers::None && !C.IsEmpty())) return false;
  W.Put<uint64>(C.Serial); W.Put<int64>(C.Reserved); W.Put<int64>(C.ObservedCapacity); W.Put<uint32>(uint32(C.Request)); W.Put<uint8>(uint8(C.Alignment)); W.Put<uint8>(uint8(C.Ledgers)|(uint8(C.Kind)<<2)|(uint8(C.State)<<5));
 }
 return W.Valid && W.Offset==890;
}
bool Decode(const FCompactTerminalLedgerImage& In,FBothLedgerImage& I)
{
 FByteReader R{In.Bytes}; if (R.Get<uint8>()!=1 || R.Get<uint8>()!=0x0f) return false;
 I=FBothLedgerImage{}; I.GateWord=FReservationDomain::ClosedBit;
 I.Identity.DomainSerial=R.Get<uint64>();
 I.Identity.PendingLedgerSerial=R.Get<uint64>();
 I.Identity.StoreLedgerSerial=R.Get<uint64>();
 I.Identity.MoveSerial=R.Get<uint64>();
 I.Identity.NextClaimSerial=R.Get<uint64>();
 I.Identity.NextBankSerial=R.Get<uint64>();
 I.Identity.AdmissionEpoch=R.Get<uint64>();
 I.Identity.StagingSerial=R.Get<uint64>();
 I.Identity.NextGenerationSerial=R.Get<uint64>();
 I.Identity.LastBindingSerial=R.Get<uint64>();
 I.Identity.ActiveBankSerial=R.Get<uint64>();
 I.Identity.Effective.StoreBytes=R.Get<int64>();
 I.Identity.Effective.MaxRecords=R.Get<int32>();
 I.Identity.Effective.BodyBytes=R.Get<int32>();
 I.Identity.Effective.BinaryBytes=R.Get<int32>();
 I.Identity.Effective.FrameBytes=R.Get<int32>();
 I.Identity.Effective.MaxFramesPerSocket=R.Get<int32>();
 I.Identity.Effective.PendingEvents=R.Get<int32>();
 I.Identity.Effective.PendingBytes=R.Get<int64>();
 I.Identity.Requested.StoreBytes=R.Get<int64>();
 I.Identity.Requested.MaxRecords=R.Get<int32>();
 I.Identity.Requested.BodyBytes=R.Get<int32>();
 I.Identity.Requested.BinaryBytes=R.Get<int32>();
 I.Identity.Requested.FrameBytes=R.Get<int32>();
 I.Identity.Requested.MaxFramesPerSocket=R.Get<int32>();
 I.Identity.Requested.PendingEvents=R.Get<int32>();
 I.Identity.Requested.PendingBytes=R.Get<int64>();
 I.PendingTotal=R.Get<int64>();
 I.PendingPeak=R.Get<int64>();
 I.StoreTotal=R.Get<int64>();
 I.StorePeak=R.Get<int64>();
 I.SnapshotControllers=R.Get<int64>();
 I.Pending.Count=R.Get<int64>();
 I.Pending.Bytes=R.Get<int64>();
 I.Pending.PeakCount=R.Get<int64>();
 I.Pending.PeakBytes=R.Get<int64>();
 I.Pending.ExcerptReservations=R.Get<int64>();
 I.Pending.DroppedCount=R.Get<uint64>();
 I.Pending.DroppedBytes=R.Get<uint64>();
 I.Pending.MetadataOmitted=R.Get<uint64>();
 I.Pending.ExcerptsSkipped=R.Get<uint64>();
 I.Pending.LossEpoch=R.Get<uint64>();
 I.ContextProbe.LiveBacking=R.Get<int64>();
 I.ContextProbe.PeakBacking=R.Get<int64>();
 I.ContextProbe.TextBytesCopied=R.Get<int64>();
 I.ContextProbe.ExcerptRefsAcquired=R.Get<int64>();
 I.ContextProbe.OverlapObservations=R.Get<int64>();
 I.ContextProbe.LiveControllers=R.Get<int64>();
 I.ContextProbe.DestroyedControllers=R.Get<int64>();
 I.ContextProbe.UnsupportedAllocator=R.Get<uint64>();
 I.ContextProbe.BoundFailures=R.Get<uint64>();
 I.ContextProbe.CalibrationRequest=R.Get<int64>();
 I.ContextProbe.CalibrationAlignment=R.Get<int64>();
 I.ContextProbe.CalibrationCapacity=R.Get<int64>();
 I.StoreProbe.LiveBacking=R.Get<int64>();
 I.StoreProbe.PeakBacking=R.Get<int64>();
 I.StoreProbe.TextBytesCopied=R.Get<int64>();
 I.StoreProbe.ExcerptRefsAcquired=R.Get<int64>();
 I.StoreProbe.OverlapObservations=R.Get<int64>();
 I.StoreProbe.LiveControllers=R.Get<int64>();
 I.StoreProbe.DestroyedControllers=R.Get<int64>();
 I.StoreProbe.UnsupportedAllocator=R.Get<uint64>();
 I.StoreProbe.BoundFailures=R.Get<uint64>();
 I.StoreProbe.CalibrationRequest=R.Get<int64>();
 I.StoreProbe.CalibrationAlignment=R.Get<int64>();
 I.StoreProbe.CalibrationCapacity=R.Get<int64>();

 const uint8 BA=R.Get<uint8>(),LA=R.Get<uint8>(); if ((BA&128)||(LA&128)) return false;
 I.BootstrapAdapter=UnpackAdapter(BA); I.LifetimeAdapter=UnpackAdapter(LA);
 for (auto& C:I.Identity.Certificates)
 {
  C.Capacity=SIZE_T(R.Get<uint64>()); C.Request=R.Get<uint16>(); C.TypeAlignment=R.Get<uint8>(); C.OrdinaryAlignment=R.Get<uint8>();
  const uint8 Flags=R.Get<uint8>(); const auto A=UnpackAdapter(Flags&127); C.Support=A.Support; C.Attempted=A.CalibrationAttempted; C.Failed=A.Failed; C.CertifiedBound=(Flags&128)?4096:0;
 }
 for (auto& B:I.Identity.Banks)
 { B.Serial=R.Get<uint64>(); B.BudgetIdentity=R.Get<uint64>(); B.CountLimit=R.Get<int64>(); B.ByteLimit=R.Get<int64>(); B.BudgetLimit=R.Get<int64>(); B.BootstrapBytes=R.Get<int64>(); B.Phase=EBankPhase(R.Get<uint8>()); }
 I.Identity.CumulativeRefusals.DroppedEvents=R.Get<uint64>();
 I.Identity.CumulativeRefusals.DroppedBytes=R.Get<uint64>();
 I.Identity.CumulativeRefusals.RefusedRows=R.Get<uint64>();
 I.Identity.CumulativeRefusals.MetadataOmitted=R.Get<uint64>();
 I.Identity.CumulativeRefusals.UnsupportedAllocator=R.Get<uint64>();
 I.Identity.CumulativeRefusals.InterruptedRows=R.Get<uint64>();
 I.Identity.CumulativeRefusals.AttachLosses=R.Get<uint64>();
 I.Identity.CumulativeRefusals.OmittedClients=R.Get<uint64>();
 I.Identity.CumulativeRefusals.ContractViolations=R.Get<uint64>();
 I.Identity.CumulativeRefusals.OmittedStatsGroups=R.Get<uint64>();
 I.Identity.CumulativeRefusals.OmittedLinks=R.Get<uint64>();
 I.Identity.CumulativeRefusals.OmittedMarkers=R.Get<uint64>();
 I.Identity.CumulativeRefusals.OmittedScopes=R.Get<uint64>();
 I.Identity.CumulativeRefusals.ExcerptsSkipped=R.Get<uint64>();

 I.ConstructedSlotMask=R.Get<uint16>(); I.Identity.ActiveBankSlot=R.Get<uint8>(); I.Identity.PolicyBits=R.Get<uint8>();
 const uint8 F=R.Get<uint8>(),P=R.Get<uint8>(); if (F&224) return false;
 I.PublicStringsUnsupported=(F&1)!=0; I.Identity.LossSaturated=(F&2)!=0; I.Identity.StickyRefusal=(F&4)!=0; I.Identity.RequestedPending=(F&8)!=0; I.Identity.Closed=(F&16)!=0;
 I.Identity.Place=ELedgerPlace(P&3); I.Identity.AuthorityPhase=EAuthorityPhase((P>>2)&7); I.Identity.DomainPhase=EDomainPhase((P>>5)&7);
 for (uint8 N=0;N<2;++N)
 {
  auto& C=I.Claims[N]; C.Serial=R.Get<uint64>(); C.Reserved=R.Get<int64>(); C.ObservedCapacity=R.Get<int64>(); C.Request=R.Get<uint32>(); C.Alignment=R.Get<uint8>();
  const uint8 Tags=R.Get<uint8>(); if (Tags&128) return false; C.Ledgers=ELedgers(Tags&3); C.Kind=EClaimKind((Tags>>2)&7); C.State=EClaimState((Tags>>5)&3);
 }
 FCompactTerminalLedgerImage RoundTrip;
 return R.Valid && R.Offset==890 && Encode(I,RoundTrip) && FMemory::Memcmp(In.Bytes,RoundTrip.Bytes,890)==0;
}
using FCallbackInstance=TBaseStaticDelegateInstance<void(),FDefaultDelegateUserPolicy,FCallbackArgument>;
constexpr SIZE_T CallbackRequest=((sizeof(FCallbackInstance)+sizeof(FAlignedInlineDelegateType)-1)/sizeof(FAlignedInlineDelegateType))*sizeof(FAlignedInlineDelegateType);
class FCheckedSimpleDelegate:public FSimpleDelegate
{
public: const void* Allocation() const { return FDefaultDelegateUserPolicy::FDelegateExtras::GetDelegateInstanceProtected(); }
};
template<class T,class D> using TController=SharedPointerInternals::TReferenceControllerWithDeleter<T,D,ESPMode::ThreadSafe>;
template<class T,class D> bool ControllerShape()
{ using C=TController<T,D>; return sizeof(C)>0 && sizeof(C)<=1024 && alignof(C)<=16 && __STDCPP_DEFAULT_NEW_ALIGNMENT__<=16; }
template<class T,class D> bool CalibrateShape(FAllocationAdapter& A,FReservationDomain& Domain,EControllerShape Shape)
{ using C=TController<T,D>; return ControllerShape<T,D>() && A.Calibrate(Domain,Shape,sizeof(C),alignof(C)); }
void SetSlot(FReservationDomain& D,EManualSlot S) { D.ConstructedSlotMask.fetch_or(uint16(1u<<uint8(S))); }
// No hidden callable: closed/actual-free bookkeeping is concrete and guarded by a copied weak.
struct FCallbackFrame
{
 FCallbackState* State;
 explicit FCallbackFrame(FCallbackState* S):State(S) { ++State->FrameDepth; State->Domain->AddRole(-1,uint8(ERootRole::CallbackFrame),true,1); State->Domain->AddRole(-1,uint8(ERootRole::DelegateArgument),true,1); }
 ~FCallbackFrame() { State->Domain->AddRole(-1,uint8(ERootRole::DelegateArgument),true,-1); State->Domain->AddRole(-1,uint8(ERootRole::CallbackFrame),true,-1); --State->FrameDepth; }
};
} // namespace

bool EqualAuthorityImages(const FBothLedgerImage& A,const FBothLedgerImage& B) { return SameImage(A,B); }
namespace
{
[[noreturn]] void TerminalInvariantFailure(const TCHAR* Reason)
{
 // Never free the sole source or overwrite its last valid image on invariant failure.
 // This unconditional stop also applies when DO_CHECK is disabled; it is NOT a semantic RED oracle.
 UE_LOG(LogTemp,Error,TEXT("Inspector terminal authority invariant: %s"),Reason);
 std::abort();
}
void CommitReceiptImage(const FBothLedgerImage& I,FCompactTerminalLedgerImage& Destination)
{
 FCompactTerminalLedgerImage Candidate; FBothLedgerImage Decoded;
 if (!Encode(I,Candidate) || !Decode(Candidate,Decoded) || !SameImage(I,Decoded)) TerminalInvariantFailure(TEXT("lossless receipt update"));
 Destination=Candidate; // The old authority survives unchanged until all validation succeeds.
}
}
bool FRootRetireReceipt::Receive(FReservationDomain& D)
{
 if (!D.RolesZero() || D.GateWord.load()!=FReservationDomain::ClosedBit || D.Identity.MoveSerial==MAX_uint64) return false;
 FBothLedgerImage I=D.Export(); ++I.Identity.MoveSerial; I.Identity.Place=ELedgerPlace::Receipt;
 FCompactTerminalLedgerImage Candidate; FBothLedgerImage Decoded;
 if (!Encode(I,Candidate) || !Decode(Candidate,Decoded) || !SameImage(I,Decoded)) return false;
 Phase.store(EReceiptPhase::Receiving,std::memory_order_release); D.Identity.AuthorityPhase=EAuthorityPhase::Frozen; Image=Candidate;
 D.Identity.AuthorityPhase=EAuthorityPhase::Moved;
 Phase.store(EReceiptPhase::OwnsBacking,std::memory_order_release); return true;
}
void FRootRetireReceipt::BackingFreed(int64 Actual)
{
 FBothLedgerImage I;
 if (Phase.load()!=EReceiptPhase::OwnsBacking || !Decode(Image,I) || I.Claims[0].State!=EClaimState::BackingLive || Actual<0 || I.ContextProbe.LiveBacking<Actual || I.PendingTotal<I.Claims[0].Reserved || I.StoreTotal<I.Claims[0].Reserved) TerminalInvariantFailure(TEXT("post-free backing receipt"));
 I.ContextProbe.LiveBacking-=Actual; I.PendingTotal-=I.Claims[0].Reserved; I.StoreTotal-=I.Claims[0].Reserved;
 I.Claims[0]=FRootClaim{}; I.ConstructedSlotMask=0; I.Identity.DomainPhase=EDomainPhase::Retired;
 CommitReceiptImage(I,Image); Phase.store(EReceiptPhase::PayloadFreed,std::memory_order_release);
}
void FRootRetireReceipt::ControllerRetired()
{
 FBothLedgerImage I;
 if (Phase.load()!=EReceiptPhase::PayloadFreed || !Decode(Image,I) || I.Claims[1].Reserved!=4096 || I.ContextProbe.LiveControllers!=1 || I.PendingTotal<I.Claims[1].Reserved || I.StoreTotal<I.Claims[1].Reserved) TerminalInvariantFailure(TEXT("final-weak controller receipt"));
 I.PendingTotal-=I.Claims[1].Reserved; I.StoreTotal-=I.Claims[1].Reserved;
 --I.ContextProbe.LiveControllers;
 uint64 DestroyedBits=0; FMemory::Memcpy(&DestroyedBits,&I.ContextProbe.DestroyedControllers,sizeof(DestroyedBits)); ++DestroyedBits; FMemory::Memcpy(&I.ContextProbe.DestroyedControllers,&DestroyedBits,sizeof(DestroyedBits));
 I.Claims[1]=FRootClaim{};
 if (I.PendingTotal!=0 || I.StoreTotal!=0 || I.ContextProbe.LiveBacking!=0 || I.Pending.Count!=0 || I.Pending.Bytes!=0 || I.Pending.ExcerptReservations!=0 || I.SnapshotControllers!=0 || I.StoreProbe.LiveBacking!=0 || I.StoreProbe.LiveControllers!=0) TerminalInvariantFailure(TEXT("nonzero final authority"));
 I.Identity.AuthorityPhase=EAuthorityPhase::Closed; CommitReceiptImage(I,Image); Phase.store(EReceiptPhase::Closed,std::memory_order_release);
}
bool FRootRetireReceipt::ReadFreed(FBothLedgerImage& Out) const
{ return Phase.load(std::memory_order_acquire)==EReceiptPhase::PayloadFreed && Decode(Image,Out); }
FRootStoredDeleter::FRootStoredDeleter(FRootStoredDeleter&& O) noexcept:bStored(true)
{
 check(!O.bStored && O.Publish); FRootPublishSlot* P=O.Publish; new (&Receipt) FRootRetireReceipt();
 P->Domain->ObserveClaim(1,-1); P->Domain->ContextProbe.LiveControllers.fetch_add(1); P->Receipt=&Receipt; O.Publish=nullptr;
}
FRootStoredDeleter::~FRootStoredDeleter() { if (bStored) { Receipt.ControllerRetired(); Receipt.~FRootRetireReceipt(); } }
void FRootStoredDeleter::operator()(FProductionRootBlock* Root)
{
 FReservationDomain* D=Root->Domain.Get(); D->Close();
 const uint16 Mask=D->ConstructedSlotMask.load(); const uint16 Expected=uint16((1u<<uint8(EManualSlot::Domain))|(1u<<uint8(EManualSlot::State)));
 if ((Mask&~Expected)!=0 || !D->RolesZero() || D->GateWord.load()!=FReservationDomain::ClosedBit) TerminalInvariantFailure(TEXT("last-strong exclusion"));
 if (Mask&(1u<<uint8(EManualSlot::State)))
 {
  FCallbackState* State=Root->State.Get(); if (State->EndFrame.IsValid() || State->PreExit.IsValid() || State->FrameDepth!=0) TerminalInvariantFailure(TEXT("callback state still owned"));
  State->~FCallbackState(); D->ConstructedSlotMask.fetch_and(uint16(0xffffu^(1u<<uint8(EManualSlot::State))));
 }
 for (uint8 N=2;N<16;++N) if (D->Claims[N].State==EClaimState::Reserved) D->RetireClaim(N);
 D->Identity.DomainPhase=EDomainPhase::Retiring; D->Identity.Closed=true;
 const int64 Actual=D->Claims[0].ObservedCapacity;
 if (!Receipt.Receive(*D)) TerminalInvariantFailure(TEXT("terminal transfer refused; source retained"));
 D->~FReservationDomain(); Root->~FProductionRootBlock();
 FMemory::Free(Root); // External inline receipt is the ONLY surviving authority.
 Receipt.BackingFreed(Actual);
}

double FMonotonicReceiptClock::Now() const { return FPlatformTime::Seconds(); }
void FOverflowRecovery::Observe(uint64 Loss,uint64 Orphan,uint64 Published)
{
 const bool Dirty=Loss!=HandledLoss || Orphan!=HandledOrphan || Loss==MAX_uint64 || Orphan==MAX_uint64;
 if (!bPending && Dirty) { TargetLoss=Loss; TargetOrphan=Orphan; Watermark=Published; bPending=true; }
}
bool FOverflowRecovery::CanRun(double Now,uint64 Consumed) const
{ return bPending && Consumed>=Watermark && FMath::IsFinite(Now) && Now-LastRun>=0.250; }
void FOverflowRecovery::Complete(double Now) { check(bPending); HandledLoss=TargetLoss; HandledOrphan=TargetOrphan; LastRun=Now; bPending=false; }

bool FService::InitialCapturePolicy(bool bEditor,bool bUnattended,bool bEditorSetting,bool bPackagedSetting,const TCHAR* CommandLine)
{
 int32 Explicit=0;
 if (FParse::Value(CommandLine,TEXT("VaRestInspector="),Explicit)) return Explicit!=0;
 if (FParse::Param(CommandLine,TEXT("VaRestInspector"))) return true;
 return bEditor ? (!bUnattended && bEditorSetting) : bPackagedSetting;
}
namespace
{
// Stages are refusal points before real operations, never qualification/enable flags.
enum class EStage:uint8 { None,BootstrapCalibration,RootBacking,RootController,Lifetime,PendingController,ClockController,ServiceController,FixedStorage,FrameBinding,PreExitBinding,Attach,Staging };
bool Refuses(uint8 Requested,EStage At)
{
#if WITH_DEV_AUTOMATION_TESTS
 return Requested==uint8(At);
#else
 return false;
#endif
}
template<class T> TSharedPtr<T,ESPMode::ThreadSafe> WrapEmbedded(T* P,const TSharedPtr<FProductionRootBlock,ESPMode::ThreadSafe>& Root,FReservationDomain& D,EManualSlot Slot,uint8 Claim,int8 Bank,uint8 PayloadRole,uint8 ControllerRole,bool bRootRole)
{
 SetSlot(D,Slot);
 if (PayloadRole<(bRootRole?uint8(ERootRole::Count):uint8(EBankRole::Count))) D.AddRole(Bank,PayloadRole,bRootRole,1);
 FRootRegionLease Lease(Root,D,Bank,ControllerRole,bRootRole);
 TRegionStoredDeleter<T> Deleter(MoveTemp(Lease),Slot,Claim,Bank,PayloadRole,bRootRole);
 LLM_SCOPE_BYTAG(VaRest_Inspector);
 return TSharedPtr<T,ESPMode::ThreadSafe>(P,MoveTemp(Deleter));
}
void SetBank(FReservationDomain& D,uint8 B,const FNormalizedLimits& L,int64 Bootstrap)
{
 auto& Bank=D.Identity.Banks[B]; check(D.BankReusable(B) && D.Identity.NextBankSerial!=MAX_uint64);
 Bank=FBankImage{}; Bank.Serial=++D.Identity.NextBankSerial; Bank.BudgetIdentity=FBudgetAccount::AllocateIdentity();
 Bank.CountLimit=L.PendingEvents; Bank.ByteLimit=L.PendingBytes; Bank.BudgetLimit=L.StoreBytes; Bank.BootstrapBytes=Bootstrap; Bank.Phase=EBankPhase::Constructing;
}
}
FService::FService(FReservationDomain& D,FCallbackState& C,const FCapturePolicy& P,TRefCountPtr<FAllocationLifetime> L,TRefCountPtr<FBudgetAccount> B,TSharedRef<FPendingAccount,ESPMode::ThreadSafe> A,TSharedRef<FMonotonicReceiptClock,ESPMode::ThreadSafe> ClockRef)
 : Domain(&D),CallbackState(&C),Policy(P),RequestedPolicy(P),Lifetime(MoveTemp(L)),Budget(MoveTemp(B)),Pending(MoveTemp(A)),Clock(MoveTemp(ClockRef)),Store(P.Limits,FProductionClockTag{},Clock)
{ Control.Emplace(*Budget); }
FService::~FService() { check(IsInGameThread()); ShutdownNow(); }
bool FService::TryCreate(const FCapturePolicy& P,TSharedPtr<FService,ESPMode::ThreadSafe>& Out)
{ return Create(P,FProductionClockSelection{},0,Out); }
#if WITH_DEV_AUTOMATION_TESTS
bool FService::TryCreateForTest(const FCapturePolicy& P,const FServiceTestOptions& O,TSharedPtr<FService,ESPMode::ThreadSafe>& Out)
{ return Create(P,{O.bDeterministicClock},uint8(O.RefuseBefore),Out,O.Witness); }
#endif
bool FService::Create(const FCapturePolicy& P,FProductionClockSelection ClockSelection,uint8 Failure,TSharedPtr<FService,ESPMode::ThreadSafe>& Out
#if WITH_DEV_AUTOMATION_TESTS
 ,FServiceFactoryWitness* Witness
#endif
 )
{
 check(IsInGameThread());
 const FSupportedAllocator Facts=InspectCurrentAllocatorForFactory();
 if (Facts.Family==FSupportedAllocator::EFamily::Unsupported || Facts.bCalibrationPassed || !Facts.bVerifiedModuleNew || !Facts.bVerifiedNoInflatingProxy || sizeof(void*)!=8 || sizeof(FProductionRootBlock)>SIZE_T(FNormalizedLimits::MaxMetadataBytes) || alignof(FProductionRootBlock)>16 || !std::is_same_v<FDelegateAllocatorType,FHeapAllocator> || sizeof(FAlignedInlineDelegateType)!=16 || alignof(FAlignedInlineDelegateType)!=16) return false;
#if UE_ENABLE_ARRAY_SLACK_TRACKING
 return false;
#endif
 if (!ControllerShape<FProductionRootBlock,FRootStoredDeleter>() || !ControllerShape<FPendingAccount,TRegionStoredDeleter<FPendingAccount>>() || !ControllerShape<FService,TRegionStoredDeleter<FService>>() || !ControllerShape<FMonotonicReceiptClock,TRegionStoredDeleter<FMonotonicReceiptClock>>() || !ControllerShape<FCaptureQueue,TPendingOwnerDeleter<FCaptureQueue>>() || !ControllerShape<FSink,TPendingOwnerDeleter<FSink>>()) return false;
#if WITH_DEV_AUTOMATION_TESTS
 if (ClockSelection.bDeterministic && !ControllerShape<FDeterministicReceiptClock,TRegionStoredDeleter<FDeterministicReceiptClock>>()) return false;
#endif
 FReservationDomain Bootstrap(P,Facts); FAllocationAdapter BootstrapAllocator(Bootstrap.ContextProbe,Bootstrap.BootstrapAdapter,Facts);
#if WITH_DEV_AUTOMATION_TESTS
 ON_SCOPE_EXIT {
  if (Witness && !Witness->bRootCreated)
  {
   // Unpublished stack authority only; no source cell is read after its move.
   Witness->BootstrapFinal=Bootstrap.Export(); Witness->bBootstrapCaptured=true;
  }
 };
#endif
 if (Refuses(Failure,EStage::BootstrapCalibration) || !CalibrateShape<FProductionRootBlock,FRootStoredDeleter>(BootstrapAllocator,Bootstrap,EControllerShape::Root)) return false;
 const auto RootBound=BootstrapAllocator.Bound(sizeof(FProductionRootBlock),alignof(FProductionRootBlock)); const auto BindingBound=BootstrapAllocator.Bound(CallbackRequest,16);
 if (!RootBound.IsSet() || !BindingBound.IsSet() || RootBound.GetValue()>SIZE_T(MAX_int64) || BindingBound.GetValue()>SIZE_T(MAX_int64)) return false;
 int64 B=int64(RootBound.GetValue());
 if (!AddChecked(B,4*4096,MAX_int64) || !AddChecked(B,int64(BindingBound.GetValue()),MAX_int64) || !AddChecked(B,int64(BindingBound.GetValue()),MAX_int64) || B>Bootstrap.PendingCap()-4096 || B>Bootstrap.StoreCap()-4096) return false;
 const auto Plan=FStore::MeasureProductionStoragePlan(P.Limits,BootstrapAllocator,B); const auto QueueBase=FCaptureQueue::InternalBaselineCharge(BootstrapAllocator);
 const auto QueuePayload=BootstrapAllocator.Bound(sizeof(FCaptureQueue),alignof(FCaptureQueue)),SinkPayload=BootstrapAllocator.Bound(sizeof(FSink),alignof(FSink)),Packet=BootstrapAllocator.Bound(sizeof(FPacketFixed),alignof(FPacketFixed));
 int64 PendingMinimum=B;
 if (!Plan.bSupported || !Plan.bFits || !QueueBase.IsSet() || !QueuePayload.IsSet() || !SinkPayload.IsSet() || !Packet.IsSet() || P.Limits.PendingEvents<1 || !AddChecked(PendingMinimum,QueueBase.GetValue(),MAX_int64) || !AddChecked(PendingMinimum,int64(QueuePayload.GetValue()),MAX_int64) || !AddChecked(PendingMinimum,int64(SinkPayload.GetValue()),MAX_int64) || !AddChecked(PendingMinimum,2*4096,MAX_int64) || !AddChecked(PendingMinimum,int64(Packet.GetValue()),MAX_int64) || !AddChecked(PendingMinimum,int64(BootstrapAllocator.QueueNodeBound()),MAX_int64) || PendingMinimum>P.Limits.PendingBytes) return false;
 if (Refuses(Failure,EStage::RootBacking)) return false;
 if (!Bootstrap.ReserveClaim(0,EClaimKind::RootBacking,sizeof(FProductionRootBlock),alignof(FProductionRootBlock),int64(RootBound.GetValue()),ELedgers::Both) || !Bootstrap.ReserveClaim(1,EClaimKind::RootController,sizeof(TController<FProductionRootBlock,FRootStoredDeleter>),alignof(TController<FProductionRootBlock,FRootStoredDeleter>),4096,ELedgers::Both))
 { for (uint8 N=0;N<16;++N) if (!Bootstrap.Claims[N].IsEmpty()) Bootstrap.RetireClaim(N); return false; }
 const SIZE_T Requests[6]={sizeof(TController<FService,TRegionStoredDeleter<FService>>),0,sizeof(TController<FPendingAccount,TRegionStoredDeleter<FPendingAccount>>),0,CallbackRequest,CallbackRequest};
 SIZE_T ClockRequest=sizeof(TController<FMonotonicReceiptClock,TRegionStoredDeleter<FMonotonicReceiptClock>>); uint32 ClockAlignment=alignof(TController<FMonotonicReceiptClock,TRegionStoredDeleter<FMonotonicReceiptClock>>);
#if WITH_DEV_AUTOMATION_TESTS
 if (ClockSelection.bDeterministic) { ClockRequest=sizeof(TController<FDeterministicReceiptClock,TRegionStoredDeleter<FDeterministicReceiptClock>>); ClockAlignment=alignof(TController<FDeterministicReceiptClock,TRegionStoredDeleter<FDeterministicReceiptClock>>); }
#endif
 bool bClaims=Bootstrap.ReserveClaim(2,EClaimKind::ServiceController,Requests[0],alignof(TController<FService,TRegionStoredDeleter<FService>>),4096,ELedgers::Both) && Bootstrap.ReserveClaim(3,EClaimKind::ClockController,ClockRequest,ClockAlignment,4096,ELedgers::Both) && Bootstrap.ReserveClaim(4,EClaimKind::PendingController,Requests[2],alignof(TController<FPendingAccount,TRegionStoredDeleter<FPendingAccount>>),4096,ELedgers::Both) && Bootstrap.ReserveClaim(6,EClaimKind::FrameBinding,CallbackRequest,16,int64(BindingBound.GetValue()),ELedgers::Both) && Bootstrap.ReserveClaim(7,EClaimKind::PreExitBinding,CallbackRequest,16,int64(BindingBound.GetValue()),ELedgers::Both);
 if (!bClaims || Refuses(Failure,EStage::RootController)) { for (uint8 N=0;N<16;++N) if (!Bootstrap.Claims[N].IsEmpty()) Bootstrap.RetireClaim(N); return false; }
 void* Raw=BootstrapAllocator.AllocateReserved(sizeof(FProductionRootBlock),alignof(FProductionRootBlock),RootBound.GetValue());
 if (!Raw) { for (uint8 N=0;N<16;++N) if (!Bootstrap.Claims[N].IsEmpty()) Bootstrap.RetireClaim(N); return false; }
 Bootstrap.ObserveClaim(0,int64(FMemory::GetAllocSize(Raw)));
 auto* Root=new (Raw) FProductionRootBlock; auto* D=new (Root->Domain.Bytes) FReservationDomain;
 if (!D->Adopt(Bootstrap,ELedgerPlace::Domain)) { D->~FReservationDomain(); Root->~FProductionRootBlock(); BootstrapAllocator.Free(Raw); for (uint8 N=0;N<16;++N) if (!Bootstrap.Claims[N].IsEmpty()) Bootstrap.RetireClaim(N); return false; }
 SetSlot(*D,EManualSlot::Domain);
 FRootPublishSlot Publish; Publish.Domain=D;
 TSharedPtr<FProductionRootBlock,ESPMode::ThreadSafe> RootOwner(Root,FRootStoredDeleter(Publish));
#if WITH_DEV_AUTOMATION_TESTS
 if (Witness) { Witness->Root=RootOwner; Witness->Receipt=Publish.Receipt; Witness->bRootCreated=true; }
#endif
 auto* State=new (Root->State.Bytes) FCallbackState; State->Domain=D; SetSlot(*D,EManualSlot::State);
 if (Refuses(Failure,EStage::Lifetime)) return false;
 FRootRegionLease ContextLease(RootOwner,*D,-1,uint8(ERootRole::ContextPayload),true);
 TRefCountPtr<FAllocationLifetime> L(new (Root->Lifetime.Bytes) FAllocationLifetime(*D,Facts,MoveTemp(ContextLease))); SetSlot(*D,EManualSlot::Lifetime);
 if (!CalibrateShape<FPendingAccount,TRegionStoredDeleter<FPendingAccount>>(L->Allocator,*D,EControllerShape::Pending) || !CalibrateShape<FService,TRegionStoredDeleter<FService>>(L->Allocator,*D,EControllerShape::Service) || !CalibrateShape<FCaptureQueue,TPendingOwnerDeleter<FCaptureQueue>>(L->Allocator,*D,EControllerShape::Queue) || !CalibrateShape<FSink,TPendingOwnerDeleter<FSink>>(L->Allocator,*D,EControllerShape::Sink)) return false;
 bool bClockCalibrated=false;
#if WITH_DEV_AUTOMATION_TESTS
 if (ClockSelection.bDeterministic) bClockCalibrated=CalibrateShape<FDeterministicReceiptClock,TRegionStoredDeleter<FDeterministicReceiptClock>>(L->Allocator,*D,EControllerShape::DeterministicClock);
 else
#endif
 bClockCalibrated=CalibrateShape<FMonotonicReceiptClock,TRegionStoredDeleter<FMonotonicReceiptClock>>(L->Allocator,*D,EControllerShape::Clock);
 if (!bClockCalibrated || !FStore::CalibrateRows(*L,*D)) return false;
 SetBank(*D,0,P.Limits,B);
 if (!D->Identity.Banks[0].BudgetIdentity) return false;
 FRootRegionLease BudgetLease(RootOwner,*D,0,uint8(EBankRole::BudgetPayload),false);
 TRefCountPtr<FBudgetAccount> A(new (Root->Budget[0].Bytes) FBudgetAccount(*D,0,L,MoveTemp(BudgetLease))); SetSlot(*D,EManualSlot::BudgetA);
 if (Refuses(Failure,EStage::PendingController)) return false;
 auto PendingOwner=WrapEmbedded(new (Root->Pending[0].Bytes) FPendingAccount(*D,0,L),RootOwner,*D,EManualSlot::PendingA,4,0,uint8(EBankRole::Count),uint8(EBankRole::PendingController),false);
 if (Refuses(Failure,EStage::ClockController)) return false;
 TSharedPtr<FMonotonicReceiptClock,ESPMode::ThreadSafe> ClockOwner;
#if WITH_DEV_AUTOMATION_TESTS
 if (ClockSelection.bDeterministic) ClockOwner=WrapEmbedded(new (Root->Clock.Deterministic.Bytes) FDeterministicReceiptClock,RootOwner,*D,EManualSlot::Clock,3,-1,uint8(ERootRole::ClockPayload),uint8(ERootRole::ClockController),true);
 else
#endif
 ClockOwner=WrapEmbedded(new (Root->Clock.Default.Bytes) FMonotonicReceiptClock,RootOwner,*D,EManualSlot::Clock,3,-1,uint8(ERootRole::ClockPayload),uint8(ERootRole::ClockController),true);
 if (Refuses(Failure,EStage::ServiceController)) return false;
 auto* Object=new (Root->Service.Bytes) FService(*D,*State,P,L,A,PendingOwner.ToSharedRef(),ClockOwner.ToSharedRef());
 auto Service=WrapEmbedded(Object,RootOwner,*D,EManualSlot::Service,2,-1,uint8(ERootRole::ServicePayload),uint8(ERootRole::ServiceController),true);
 Service->Receipt=Publish.Receipt; Service->Self=Service; Service->RefuseBefore=Failure;
 if (Refuses(Failure,EStage::FixedStorage) || !Service->Store.Initialize(A) || !Service->Store.BindLossWitness(PendingOwner.ToSharedRef()) || !Service->BindCallbacks()) return false;
 D->Identity.Banks[0].Phase=EBankPhase::Active; D->Identity.ActiveBankSlot=0; D->Identity.ActiveBankSerial=D->Identity.Banks[0].Serial;
 State->Constructing=false; State->Closed=false;
 Out=MoveTemp(Service); return true;
}

template<class T,class... Args> TSharedPtr<T,ESPMode::ThreadSafe> FService::MakePendingOwner(EBankRole PayloadRole,EBankRole ControllerRole,Args&&... Arguments)
{
 using D=TPendingOwnerDeleter<T>; using C=TController<T,D>;
 const auto& Certificate=Domain->Identity.Certificates[uint8(std::is_same_v<T,FSink>?EControllerShape::Sink:EControllerShape::Queue)];
 if (!Pending || !Certificate.Attempted || Certificate.Failed || Certificate.Request!=sizeof(C) || Certificate.TypeAlignment!=alignof(C) || !Lifetime->Allocator.GetSupport().SupportsController(sizeof(C),alignof(C))) return {};
 const auto Bound=Lifetime->Allocator.Bound(sizeof(T),alignof(T)); if (!Bound.IsSet() || Bound.GetValue()>SIZE_T(MAX_int64)) return {};
 auto Ctrl=FPendingBaseline::Acquire(Pending.ToSharedRef(),4096); if (!Ctrl.IsSet()) return {};
 auto Payload=FPendingBaseline::Acquire(Pending.ToSharedRef(),int64(Bound.GetValue())); if (!Payload.IsSet()) return {};
 void* Memory=Lifetime->Allocator.AllocateReserved(sizeof(T),alignof(T),Bound.GetValue()); if (!Memory) return {};
 T* Object=new (Memory) T(Forward<Args>(Arguments)...); Domain->AddRole(Pending->Bank,uint8(PayloadRole),false,1);
 D Deleter(Pending.ToSharedRef(),MoveTemp(Payload.GetValue()),MoveTemp(Ctrl.GetValue()),PayloadRole,ControllerRole);
 LLM_SCOPE_BYTAG(VaRest_Inspector); return TSharedPtr<T,ESPMode::ThreadSafe>(Object,MoveTemp(Deleter));
}

bool FService::BindCallback(bool bExit)
{
 check(IsInGameThread());
 if (Refuses(RefuseBefore,bExit?EStage::PreExitBinding:EStage::FrameBinding) || !Lifetime) return false;
 const uint8 Slot=bExit?7:6; const auto& Claim=Domain->Claims[Slot];
 const auto Current=InspectCurrentAllocatorForFactory(); const auto Supported=Lifetime->Allocator.GetSupport();
 const auto Bound=Lifetime->Allocator.Bound(CallbackRequest,16);
 if (Current.Family==FSupportedAllocator::EFamily::Unsupported || Current.Family!=Supported.Family ||
  Current.bCalibrationPassed || !Current.bVerifiedModuleNew || !Current.bVerifiedNoInflatingProxy ||
  !Supported.bCalibrationPassed || !Supported.bVerifiedModuleNew || !Supported.bVerifiedNoInflatingProxy ||
  !Bound.IsSet() || Bound.GetValue()>SIZE_T(MAX_int64) ||
  Claim.Serial==0 || Claim.Kind!=(bExit?EClaimKind::PreExitBinding:EClaimKind::FrameBinding) ||
  Claim.Ledgers!=ELedgers::Both || Claim.State!=EClaimState::Reserved || Claim.ObservedCapacity!=0 ||
  Claim.Request!=CallbackRequest || Claim.Alignment!=16 || Claim.Reserved!=int64(Bound.GetValue()) ||
  (bExit?CallbackState->PreExit.IsValid():CallbackState->EndFrame.IsValid())) return false;
 FCheckedSimpleDelegate Delegate;
 FCallbackArgument Argument{Self,CallbackState};
 Delegate.BindStatic(bExit?&FService::PreExit:&FService::EndFrame,MoveTemp(Argument));
 const void* Instance=Delegate.Allocation(); const SIZE_T Actual=Instance?FMemory::GetAllocSize(const_cast<void*>(Instance)):0;
 if (!Instance || Delegate.GetAllocatedSize()!=CallbackRequest || Actual<CallbackRequest || Actual>SIZE_T(Claim.Reserved))
 { Delegate.Unbind(); AddSaturated(Domain->ContextProbe.BoundFailures); return false; }
 Domain->ObserveClaim(Slot,int64(Actual));
 ObservePeak(Domain->ContextProbe.PeakBacking,Domain->ContextProbe.LiveBacking.fetch_add(int64(Actual))+int64(Actual));
 if (bExit) { CallbackState->ExitActual=int64(Actual); CallbackState->PreExit=FCoreDelegates::OnPreExit.Add(MoveTemp(static_cast<FSimpleDelegate&>(Delegate))); }
 else { CallbackState->FrameActual=int64(Actual); CallbackState->EndFrame=FCoreDelegates::OnEndFrame.Add(MoveTemp(static_cast<FSimpleDelegate&>(Delegate))); }
 if (!(bExit?CallbackState->PreExit.IsValid():CallbackState->EndFrame.IsValid()))
  TerminalInvariantFailure(TEXT("checked binding publication lost its owned handle"));
 return true;
}
bool FService::BindCallbacks()
{
 check(IsInGameThread());
 if (!BindCallback(false) || !BindCallback(true)) { RemoveCallbacks(); return false; }
 return true;
}
bool FService::EnsureFrameCallback()
{
 check(IsInGameThread());
 if (bShutdown || CallbackState->Closed || CallbackState->Constructing) return false;
 if (CallbackState->EndFrame.IsValid()) return true;
 return BindCallback(false);
}
bool FService::EnsureFrameOrShutdown()
{
 if (EnsureFrameCallback()) return true;
 // Never destroy borrowed visitor/observer state. The guaranteed outer flush
 // performs the real shutdown after the current dispatch/tick has unwound.
 if (CallbackState->FacadeClosed) *CallbackState->FacadeClosed=true;
 if (DispatchDepth || bTicking) bDeferredShutdown=true;
 else ShutdownNow();
 return false;
}
void FService::SuspendIdleFrameCallback()
{
 check(IsInGameThread());
 if (bShutdown || DispatchDepth || bTicking || bCapturing || bDirty || Store.Revision()!=NotifiedRevision ||
  bDeferredShutdown || bDeferredClear || bHasCaptureRequest || bHasPolicyRequest || !CallbackState->EndFrame.IsValid()) return;
 // A real EndFrame calls this with FrameDepth1, before its copied weak unwinds.
 const auto& Claim=Domain->Claims[6]; const auto Bound=Lifetime->Allocator.Bound(CallbackRequest,16);
 const int64 Actual=CallbackState->FrameActual;
 if (CallbackState->Closed || CallbackState->Constructing || CallbackState->FrameDepth>1 ||
  Domain->Identity.AuthorityPhase!=EAuthorityPhase::Active || Domain->Identity.Closed || Domain->Identity.Place!=ELedgerPlace::Domain ||
  (Domain->Identity.DomainPhase!=EDomainPhase::Unpublished && Domain->Identity.DomainPhase!=EDomainPhase::Live) ||
  Claim.Serial==0 || Claim.Kind!=EClaimKind::FrameBinding || Claim.Ledgers!=ELedgers::Both ||
  Claim.State!=EClaimState::BackingLive || Claim.Request!=CallbackRequest || Claim.Alignment!=16 ||
  !Bound.IsSet() || Bound.GetValue()>SIZE_T(MAX_int64) || Claim.Reserved!=int64(Bound.GetValue()) ||
  Actual<=0 || Claim.ObservedCapacity!=Actual || Actual>Claim.Reserved ||
  Domain->PendingTotal.load()<Claim.Reserved || Domain->StoreTotal.load()<Claim.Reserved ||
  Domain->ContextProbe.LiveBacking.load()<Actual)
  TerminalInvariantFailure(TEXT("idle frame reservation did not match its owned binding"));
 const FDelegateHandle Handle=CallbackState->EndFrame; CallbackState->EndFrame.Reset();
 if (!FCoreDelegates::OnEndFrame.Remove(Handle))
  TerminalInvariantFailure(TEXT("idle owned binding actual free not proved"));
 if (!Domain->RetainFrameReservationAfterFree(CallbackRequest,Actual))
  TerminalInvariantFailure(TEXT("idle freed binding could not retain its reservation"));
 CallbackState->FrameActual=0;
}
void FService::RemoveCallbacks()
{
 check(IsInGameThread());
 CallbackState->Closed=true;
 const FDelegateHandle Frame=CallbackState->EndFrame,Exit=CallbackState->PreExit;
 CallbackState->EndFrame.Reset(); CallbackState->PreExit.Reset();
 if (Frame.IsValid()) { const bool Removed=FCoreDelegates::OnEndFrame.Remove(Frame); if (!Removed) TerminalInvariantFailure(TEXT("owned binding actual free not proved")); Domain->ContextProbe.LiveBacking.fetch_sub(CallbackState->FrameActual); CallbackState->FrameActual=0; Domain->RetireClaim(6); }
 else if (Domain->Claims[6].State==EClaimState::Reserved)
 {
  if (CallbackState->FrameActual!=0 || Domain->Claims[6].ObservedCapacity!=0)
   TerminalInvariantFailure(TEXT("idle reserved binding retained physical backing"));
  Domain->RetireClaim(6);
 }
 if (Exit.IsValid()) { const bool Removed=FCoreDelegates::OnPreExit.Remove(Exit); if (!Removed) TerminalInvariantFailure(TEXT("owned binding actual free not proved")); Domain->ContextProbe.LiveBacking.fetch_sub(CallbackState->ExitActual); CallbackState->ExitActual=0; Domain->RetireClaim(7); }
}
void FService::EndFrame(FCallbackArgument Argument)
{
 if (!Argument.State) return;
 auto Service=Argument.Service.Pin(); FCallbackFrame Frame(Argument.State);
 if (!Service || Argument.State->Closed || Argument.State->Constructing || Argument.State->LastFrame==GFrameCounter) return;
 Argument.State->LastFrame=GFrameCounter; Service->Tick();
}
void FService::PreExit(FCallbackArgument Argument)
{
 if (!Argument.State) return;
 auto Service=Argument.Service.Pin(); FCallbackFrame Frame(Argument.State);
 if (Argument.State->FacadeClosed) *Argument.State->FacadeClosed=true;
 if (Service) Service->Shutdown();
}
void FService::AttachFacade(FVaRestInspectorChanged& Changed,bool* ShutdownFlag)
{ check(IsInGameThread() && !bShutdown); ChangedRoute=&Changed; CallbackState->FacadeClosed=ShutdownFlag; }
void FService::DetachFacade()
{ check(IsInGameThread()); ChangedRoute=nullptr; CallbackState->FacadeClosed=nullptr; }
void FService::MarkChanged()
{
 if (bShutdown || !EnsureFrameOrShutdown()) return;
 if (Store.RevisionValue!=MAX_uint64) ++Store.RevisionValue;
 ServiceRevision=Store.Revision(); bDirty=true;
}
uint64 FService::Revision() const { check(IsInGameThread()); return Store.Revision(); }
void FService::SetCapture(bool Enabled) { RequestControl(Enabled?EControl::Start:EControl::Pause); }
void FService::RequestPolicy(const FCapturePolicy& P)
{
 check(IsInGameThread()); if (bShutdown || !EnsureFrameOrShutdown()) return;
 RequestedPolicy=P; bHasPolicyRequest=true;
 FlushDeferredControl();
}
void FService::ApplyRequestedPolicy()
{
 const auto& L=RequestedPolicy.Limits; const auto& E=Domain->Identity.Effective;
 Domain->Identity.Requested=L;
 Domain->Identity.RequestedPending=L.StoreBytes!=E.StoreBytes || L.MaxRecords!=E.MaxRecords || L.PendingBytes!=E.PendingBytes || L.PendingEvents!=E.PendingEvents;
 Store.RequestLimits(L);
 Store.State.bPendingLimits=Domain->Identity.RequestedPending;
 bHasPolicyRequest=false;
}
bool FService::Start(const FCapturePolicy& P)
{
 check(IsInGameThread()); if (bShutdown) return false;
 if (!DispatchDepth && !bTicking && !EnsureFrameCallback()) return false;
 RequestedPolicy=P; bHasPolicyRequest=true;
 if (DispatchDepth || bTicking) { bHasCaptureRequest=true; bRequestedCapture=true; return true; }
 ApplyRequestedPolicy();
 return StartNow(RequestedPolicy);
}
bool FService::StartNow(const FCapturePolicy& P)
{
 check(IsInGameThread());
 if (bShutdown || InspectCurrentAllocatorForFactory().Family==FSupportedAllocator::EFamily::Unsupported) return false;
 if (bCapturing) return true;
 if (!(Domain->GateWord.load(std::memory_order_acquire)&FReservationDomain::ClosedBit) || !Budget || !Pending || Generation==MAX_uint64 || Domain->Identity.NextGenerationSerial==MAX_uint64) return false;
 if (!EnsureFrameCallback()) return false;
 // Adopt complete detail policy at restart; the four accounting capacities
 // remain tied to their real old accounts until explicit Clear commits.
 Policy=P;
 Domain->Identity.PolicyBits=uint8(Policy.bHeaders)|(uint8(Policy.bBodies)<<1);
 Policy.Limits.StoreBytes=Domain->Identity.Effective.StoreBytes;
 Policy.Limits.MaxRecords=Domain->Identity.Effective.MaxRecords;
 Policy.Limits.PendingBytes=Domain->Identity.Effective.PendingBytes;
 Policy.Limits.PendingEvents=Domain->Identity.Effective.PendingEvents;
 Domain->Identity.Effective.BodyBytes=Policy.Limits.BodyBytes;
 Domain->Identity.Effective.BinaryBytes=Policy.Limits.BinaryBytes;
 Domain->Identity.Effective.FrameBytes=Policy.Limits.FrameBytes;
 Domain->Identity.Effective.MaxFramesPerSocket=Policy.Limits.MaxFramesPerSocket;
 Store.Limits=Policy.Limits;
 ++Generation; ++Domain->Identity.NextGenerationSerial;
 auto Queue=MakePendingOwner<FCaptureQueue>(EBankRole::QueuePayload,EBankRole::QueueController,FQueueWrapperAdmission{},Policy,Generation,Pending.ToSharedRef(),Lifetime->Allocator);
 if (!Queue || !Queue->IsSupported()) return false;
 auto Sink=MakePendingOwner<FSink>(EBankRole::SinkPayload,EBankRole::SinkController,Policy,Generation,Queue.ToSharedRef(),Pending.ToSharedRef(),Self);
 if (!Sink) return false;
 Recovery=FOverflowRecovery{};
 Store.BeginGeneration(Generation,0);
 if (!Store.BindLossWitness(Pending.ToSharedRef()) || !Domain->Reopen(Pending->Bank)) return false;
 ActiveQueue=MoveTemp(Queue); ActiveSink=MoveTemp(Sink); bCapturing=true; Store.State.bCapturing=true;
 ++DispatchDepth;
 FVaRestTrace::AddSink(ActiveSink.ToSharedRef());
 --DispatchDepth;
 if (Refuses(RefuseBefore,EStage::Attach)) { PauseNow(); return false; }
 MarkChanged(); FlushDeferredControl();
 if (bShutdown || !bCapturing) { if (ActiveSink) { ActiveSink->Close(); FVaRestTrace::RemoveSink(ActiveSink.ToSharedRef()); } return false; }
 return true;
}
void FService::CloseGeneration()
{
 Domain->Close();
 auto Sink=MoveTemp(ActiveSink); auto Queue=MoveTemp(ActiveQueue);
 bCapturing=false; Store.State.bCapturing=false;
 if (Sink) { Sink->Close(); FVaRestTrace::RemoveSink(Sink.ToSharedRef()); }
 if (Queue) { Queue->Close(); while (Queue->TryPopMetadata().IsSet()) {} while (Queue->TryPopExcerpt().IsSet()) {} }
 Recovery=FOverflowRecovery{};
}
void FService::PauseNow()
{
 if (bShutdown) return;
 const bool Was=bCapturing; CloseGeneration(); Store.InterruptOpenRows(); Store.StopPendingExcerpts(); if (Was) MarkChanged();
}
void FService::Pause() { RequestControl(EControl::Pause); }
void FService::Clear() { RequestControl(EControl::Clear); }
void FService::BeginPIESession(bool Preserve)
{
 check(IsInGameThread()); if (bShutdown) return;
 // Drain the previous session's already-published scope closures before selecting rows.
 if (!DispatchDepth && !bTicking) Tick();
 bDeferredPIE=true; bPIEPreserve=Preserve; FlushDeferredControl();
}
bool FService::ClearScope(uint64 Id)
{
 check(IsInGameThread()); if (bShutdown || bDeferredShutdown || !EnsureFrameOrShutdown()) return false;
 if (!ScopedClear.Contains(Id)) { if (ScopedClear.Num()>=16) return false; ScopedClear.Add(Id); }
 FlushDeferredControl(); return true;
}
void FService::ClearScopeNow(uint64 Id)
{
 if (bShutdown || !Store.Storage) return;
 ++DispatchDepth;
 const auto Snapshot=FVaRestTrace::BuildSnapshot();
 if (!bDeferredShutdown)
 {
  Store.ClearScope(Id, Snapshot.InFlight, Snapshot.Sockets);
  if (bCapturing) ApplyAttached(Snapshot);
  MarkChanged();
 }
 --DispatchDepth;
}
void FService::ClearNow()
{
 if (bShutdown) return;
 const bool Resume=bCapturing; CloseGeneration();
 Store.Clear();
 if (Domain->Identity.RequestedPending)
 {
  Store.ReleaseFixedStorage();
  if (!RecreateAccounts())
  {
   if (!Store.Storage) Store.InitializeStorage(Budget);
   Store.BindLossWitness(Pending.ToSharedRef());
  }
 }
 MarkChanged();
 if (Resume && Store.Storage) StartNow(RequestedPolicy);
}
void FService::Shutdown() { RequestControl(EControl::Shutdown); }
void FService::ShutdownNow()
{
 if (bShutdown) return;
 bShutdown=true; Domain->Identity.Closed=true; DetachFacade();
 CloseGeneration(); RemoveCallbacks(); Store.InterruptOpenRows(); Store.StopPendingExcerpts(); Store.Clear(); Store.ReleaseFixedStorage(); Store.OriginatingPending.Reset();
 Control.Reset(); Pending.Reset(); Budget.SafeRelease(); Lifetime.SafeRelease();
 ScopedClear.Reset(); bDeferredPIE=false; bDeferredShutdown=bDeferredClear=bHasCaptureRequest=bHasPolicyRequest=false; bDirty=false;
}
void FService::RequestControl(EControl C)
{
 check(IsInGameThread());
 if (bShutdown) return;
 if (C!=EControl::None && C!=EControl::Shutdown && !EnsureFrameOrShutdown()) return;
 if (C==EControl::Shutdown) bDeferredShutdown=true;
 else if (C==EControl::Clear) bDeferredClear=true;
 else if (C==EControl::Start || C==EControl::Pause) { bHasCaptureRequest=true; bRequestedCapture=C==EControl::Start; }
 FlushDeferredControl();
}
void FService::FlushDeferredControl()
{
 if (DispatchDepth || bTicking || bShutdown) return;
 if (bDeferredShutdown) { bDeferredShutdown=false; ShutdownNow(); return; }
 if ((bHasPolicyRequest || bDeferredClear || bHasCaptureRequest || bDeferredPIE || !ScopedClear.IsEmpty()) && !EnsureFrameOrShutdown()) return;
 if (bHasPolicyRequest) ApplyRequestedPolicy();
 const bool ClearRequest=bDeferredClear,HasCapture=bHasCaptureRequest,Capture=bRequestedCapture;
 bDeferredClear=bHasCaptureRequest=false;
 TArray<uint64,TInlineAllocator<16>> Scopes=MoveTemp(ScopedClear); ScopedClear.Reset();
 if (ClearRequest) ClearNow();
 else for (uint64 Id:Scopes) if (!bShutdown) ClearScopeNow(Id);
 if (!bShutdown && bDeferredPIE) { bDeferredPIE=false; Store.BeginPIESession(bPIEPreserve); MarkChanged(); }
 if (!bShutdown && HasCapture) { if (Capture) StartNow(RequestedPolicy); else PauseNow(); }
}
void FService::ApplyAttached(const FVaRestTraceSnapshot& S)
{
 check(IsInGameThread()); if (bShutdown || !bCapturing || !Pending) return;
 ++DispatchDepth; const uint64 Before=Store.Revision();
 Store.ApplyAttached(S,Policy,Pending.ToSharedRef(),Lifetime->Allocator);
 Store.RecordCumulativeDiagnostics();
 if (Store.Revision()!=Before) bDirty=true;
 --DispatchDepth; // AddSink owns the outer control frame; publish only after it returns.
}
void FService::Notify()
{
 const uint64 Current=Store.Revision();
 if (!bDirty && Current==NotifiedRevision) return;
 bDirty=false; NotifiedRevision=Current;
 if (ChangedRoute && !bShutdown)
 {
  ++DispatchDepth; Domain->AddRole(-1,uint8(ERootRole::ObserverFrame),true,1);
  ChangedRoute->Broadcast(Current);
  Domain->AddRole(-1,uint8(ERootRole::ObserverFrame),true,-1); --DispatchDepth;
 }
}
void FService::Tick()
{
 check(IsInGameThread());
 if (bShutdown || bTicking) return;
 bTicking=true; ++DispatchDepth; Domain->AddRole(-1,uint8(ERootRole::ServiceFrame),true,1);
 const uint64 Before=Store.Revision();
 if (bCapturing && ActiveQueue)
 {
  const auto Observe=[&] { const auto O=Store.ObserveScheduler(); Recovery.Observe(ActiveQueue->LossEpoch(),O.OrphanEpoch,ActiveQueue->LastPublishedMetadata()); };
  Store.ObserveQueueLoss(Generation,*Pending); Observe();
  int32 Work=0;
  const auto Consume=[&](bool Metadata)->bool {
   auto Packet=Metadata?ActiveQueue->TryPopMetadata():ActiveQueue->TryPopExcerpt();
   if (!Packet.IsSet()) return false;
   ++Work; Store.Apply(MoveTemp(Packet.GetValue())); return true;
  };
  for (int32 N=0;N<192 && Consume(true);++N) {}
  for (int32 N=0;N<64 && Consume(false);++N) {}
  while (Work<256) { if (!Consume(true) && !Consume(false)) break; }
  Observe(); const double Now=Clock->Now();
  if (Recovery.CanRun(Now,Store.ObserveScheduler().LastConsumedMetadataOrdinal))
  {
   { FVaRestTraceSnapshot Source=FVaRestTrace::BuildSnapshot(); Store.ApplyAttached(Source,Policy,Pending.ToSharedRef(),Lifetime->Allocator,Recovery.Watermark); }
   LastRecoveryWatermark=Recovery.Watermark; ++RecoveryCount; Recovery.Complete(Now);
  }
  Observe(); // Quiet rearm is mandatory even if no further event arrives.
  Store.ExpirePendingExcerpts(); Store.ObserveQueueLoss(Generation,*Pending);
  if (ActiveQueue->NeedsRotation()) { bHasCaptureRequest=true; bRequestedCapture=true; CloseGeneration(); Store.InterruptOpenRows(); }
 }
 Store.RecordCumulativeDiagnostics();
 if (Store.Revision()!=Before) bDirty=true;
 Notify();
 Domain->AddRole(-1,uint8(ERootRole::ServiceFrame),true,-1); --DispatchDepth; bTicking=false;
 FlushDeferredControl();
 SuspendIdleFrameCallback();
}
bool FService::TrySnapshot(TSharedPtr<const FVaRestInspectorSnapshot,ESPMode::ThreadSafe>& Out) const
{ check(IsInGameThread()); return !bShutdown && Store.TrySnapshot(Out); }
bool FService::ForEachRecord(TFunctionRef<void(const FVaRestInspectorRecordView&)> Visitor)
{
 check(IsInGameThread()); if (bShutdown) return false;
 ++DispatchDepth; Domain->AddRole(-1,uint8(ERootRole::StoreVisitor),true,1);
 const bool Result=Store.ForEachRecord(Visitor);
 Domain->AddRole(-1,uint8(ERootRole::StoreVisitor),true,-1); --DispatchDepth; FlushDeferredControl(); return Result;
}

bool FService::ForEachLink(TFunctionRef<void(const FVaRestInspectorLinkView&)> Visitor)
{
 check(IsInGameThread()); if (bShutdown) return false;
 ++DispatchDepth; Domain->AddRole(-1,uint8(ERootRole::StoreVisitor),true,1);
 const bool Result=Store.ForEachLink(Visitor);
 Domain->AddRole(-1,uint8(ERootRole::StoreVisitor),true,-1); --DispatchDepth; FlushDeferredControl(); return Result;
}

bool FService::RecreateAccounts()
{
 check(IsInGameThread() && DispatchDepth==0 && !bTicking);
 if (!Pending || !Budget || !Domain->Identity.RequestedPending || Refuses(RefuseBefore,EStage::Staging) || InspectCurrentAllocatorForFactory().Family!=Lifetime->Allocator.GetSupport().Family) return false;
 const uint8 Old=Pending->Bank,Next=uint8(1-Old);
 if (Domain->GateWord.load(std::memory_order_acquire)!=FReservationDomain::ClosedBit || Pending->Count.load()!=0 || Pending->Bytes.load()!=0 || Pending->ExcerptReservations.load()!=0 || !Domain->BankReusable(Next)) return false;
 for (uint8 R=0;R<uint8(EBankRole::Count);++R)
 {
  const bool Private=R==uint8(EBankRole::BudgetPayload) || R==uint8(EBankRole::PendingPayload) || R==uint8(EBankRole::PendingController) || R==uint8(EBankRole::ControlLease);
  if (Domain->BankRoles[Old][R].load(std::memory_order_acquire)!=(Private?1:0)) return false;
 }
 auto Root=Lifetime->Region.PinRoot(); if (!Root) return false;
 const auto Requested=Domain->Identity.Requested; const int64 Bootstrap=Budget->BootstrapBytes;
 Domain->Identity.DomainPhase=EDomainPhase::Recreating;
 FRootRegionLease StagingRegion(Root,*Domain,-1,uint8(ERootRole::StagingPermit),true);
 bool Committed=false;
 ON_SCOPE_EXIT { if (!Committed) Domain->Identity.DomainPhase=EDomainPhase::Live; };
 if (Domain->Identity.StagingSerial==MAX_uint64 || Domain->Identity.NextBankSerial==MAX_uint64) return false;
 ++Domain->Identity.StagingSerial; FRecreationStagingPermit Permit(MoveTemp(StagingRegion),Next,Domain->Identity.StagingSerial); FRootQuiescenceProof Proof(Permit);
 SetBank(*Domain,Next,Requested,Bootstrap);
 if (!Domain->Identity.Banks[Next].BudgetIdentity) return false;
 Domain->Identity.Banks[Next].Phase=EBankPhase::Staging;
 const auto& Cert=Domain->Identity.Certificates[uint8(EControllerShape::Pending)];
 using C=TController<FPendingAccount,TRegionStoredDeleter<FPendingAccount>>;
 if (!Cert.Attempted || Cert.Failed || Cert.Request!=sizeof(C) || Cert.TypeAlignment!=alignof(C) || !Domain->ReserveClaim(uint8(4+Next),EClaimKind::PendingController,sizeof(C),alignof(C),4096,ELedgers::Both)) { Domain->Identity.Banks[Next].Phase=EBankPhase::Reusable; return false; }
 FRootRegionLease BudgetLease(Root,*Domain,Next,uint8(EBankRole::BudgetPayload),false);
 TRefCountPtr<FBudgetAccount> NewBudget(new (Root->Budget[Next].Bytes) FBudgetAccount(*Domain,Next,Lifetime,MoveTemp(BudgetLease))); SetSlot(*Domain,Next?EManualSlot::BudgetB:EManualSlot::BudgetA);
 auto NewPending=WrapEmbedded(new (Root->Pending[Next].Bytes) FPendingAccount(*Domain,Next,Lifetime),Root,*Domain,Next?EManualSlot::PendingB:EManualSlot::PendingA,uint8(4+Next),Next,uint8(EBankRole::Count),uint8(EBankRole::PendingController),false);
 auto Prepared=Store.PrepareAccountChange(Requested,NewBudget,NewPending.ToSharedRef(),Permit);
 if (!Prepared.IsSet() || Domain->StoreTotal.load()>Requested.StoreBytes || Domain->PendingTotal.load()>Requested.PendingBytes)
 {
  Prepared.Reset(); NewPending.Reset(); NewBudget.SafeRelease();
  check(Domain->BankReusable(Next)); Domain->Identity.Banks[Next].Phase=EBankPhase::Reusable; return false;
 }
 // ONE exclusion/permit spans install AND every old-handle destructor/final weak/bank proof.
 auto OldPending=MoveTemp(Pending); auto OldBudget=MoveTemp(Budget); auto OldControl=MoveTemp(Control);
 Store.CommitAccountChange(MoveTemp(Prepared.GetValue()),MoveTemp(Proof));
 Pending=NewPending; Budget=NewBudget; Control=MoveTemp(Prepared->Control); Prepared.Reset();
 Domain->Identity.Banks[Old].Phase=EBankPhase::Retiring;
 OldControl.Reset(); OldPending.Reset(); OldBudget.SafeRelease();
 check(Domain->BankReusable(Old)); Domain->Identity.Banks[Old].Phase=EBankPhase::Reusable;
 Domain->Identity.Effective=Requested; Domain->Identity.RequestedPending=false;
 Domain->Identity.ActiveBankSlot=Next; Domain->Identity.ActiveBankSerial=Domain->Identity.Banks[Next].Serial; Domain->Identity.Banks[Next].Phase=EBankPhase::Active;
 Policy.Limits=Requested; Domain->Identity.DomainPhase=EDomainPhase::Live; Committed=true;
 return true; // Gate remains CLOSED; a subsequent Start alone publishes generation admissions.
}
#if WITH_DEV_AUTOMATION_TESTS
FServiceTestImage FService::TestImage() const
{
 check(IsInGameThread()); FServiceTestImage I;
 I.RecoveryCount=RecoveryCount; I.LastRecoveryWatermark=LastRecoveryWatermark; I.Generation=Generation; I.Revision=Revision();
 I.PendingTotal=Domain->PendingTotal.load(); I.PendingDynamic=Domain->Pending.Bytes.load(); I.StoreTotal=Domain->StoreTotal.load(); I.PendingPeak=Domain->PendingPeak.load(); I.StorePeak=Domain->StorePeak.load(); I.Controllers=Domain->ContextProbe.LiveControllers.load()+Domain->StoreProbe.LiveControllers.load();
 I.EffectiveStore=Domain->Identity.Effective.StoreBytes; I.EffectivePending=Domain->Identity.Effective.PendingBytes; I.RequestedStore=Domain->Identity.Requested.StoreBytes; I.RequestedPending=Domain->Identity.Requested.PendingBytes; I.SnapshotControllers=Domain->SnapshotControllers.load(); I.LiveBacking=Domain->ContextProbe.LiveBacking.load(); I.TextCopies=Domain->ContextProbe.TextBytesCopied.load(); I.ExcerptRefs=Domain->ContextProbe.ExcerptRefsAcquired.load(); I.LossEpoch=Domain->Pending.LossEpoch.load();
 I.Entries=int64(Domain->GateWord.load()&~FReservationDomain::ClosedBit); I.bCapturing=bCapturing; I.bPendingLimits=Domain->Identity.RequestedPending; I.bShutdown=bShutdown; return I;
}
TSharedPtr<FDeterministicReceiptClock,ESPMode::ThreadSafe> FService::ClockForTest() const
{
 const auto& C=Domain->Identity.Certificates[uint8(EControllerShape::DeterministicClock)];
 if (C.Attempted && !C.Failed) return StaticCastSharedRef<FDeterministicReceiptClock>(Clock); return {};
}
TWeakPtr<FProductionRootBlock,ESPMode::ThreadSafe> FService::RootForTest() const { return Lifetime ? TWeakPtr<FProductionRootBlock,ESPMode::ThreadSafe>(Lifetime->Region.PinRoot()) : TWeakPtr<FProductionRootBlock,ESPMode::ThreadSafe>(); }
void FService::RequestLimitsForTest(const FNormalizedLimits& L)
{
 check(IsInGameThread() && !bShutdown);
 RequestedPolicy.Limits=L; bHasPolicyRequest=true; FlushDeferredControl();
}
#endif
uint32 FService::DescribeProductionTypes(FTypeLayoutRow* Out,uint32 Capacity)
{
 uint32 N=0;
 const auto Add=[&](const TCHAR* Name,SIZE_T Size,uint32 Alignment,SIZE_T Request) { if (N<Capacity) Out[N]={Name,Size,Alignment,Request}; ++N; };
#define TYPE(T) Add(TEXT(#T),sizeof(T),alignof(T),sizeof(T))
 TYPE(FBothLedgerImage); TYPE(FDomainMetadata); TYPE(FReservationDomain); TYPE(FRootClaim); TYPE(FCompactTerminalLedgerImage); TYPE(FRootRetireReceipt); TYPE(FRootStoredDeleter); TYPE(FProductionRootBlock); TYPE(FAllocationLifetime); TYPE(FBudgetAccount); TYPE(FPendingAccount); TYPE(FService); TYPE(FMonotonicReceiptClock); TYPE(FCallbackState); TYPE(FCallbackArgument); TYPE(FCallbackInstance); TYPE(FRecreationStagingPermit); TYPE(FRootQuiescenceProof);
#undef TYPE
#define CONTROLLER(T,D) Add(TEXT("Controller:" #T),sizeof(TController<T,D>),alignof(TController<T,D>),sizeof(TController<T,D>))
 CONTROLLER(FProductionRootBlock,FRootStoredDeleter); CONTROLLER(FPendingAccount,TRegionStoredDeleter<FPendingAccount>); CONTROLLER(FService,TRegionStoredDeleter<FService>); CONTROLLER(FMonotonicReceiptClock,TRegionStoredDeleter<FMonotonicReceiptClock>); CONTROLLER(FCaptureQueue,TPendingOwnerDeleter<FCaptureQueue>); CONTROLLER(FSink,TPendingOwnerDeleter<FSink>);
#if WITH_DEV_AUTOMATION_TESTS
 CONTROLLER(FDeterministicReceiptClock,TRegionStoredDeleter<FDeterministicReceiptClock>);
 Add(TEXT("FDeterministicReceiptClock"),sizeof(FDeterministicReceiptClock),alignof(FDeterministicReceiptClock),sizeof(FDeterministicReceiptClock));
#endif
#undef CONTROLLER
 Add(TEXT("ActiveClaims16"),sizeof(FBothLedgerImage::Claims),alignof(FRootClaim),sizeof(FBothLedgerImage::Claims));
 Add(TEXT("CallbackRoundedRequest"),sizeof(FCallbackInstance),alignof(FCallbackInstance),CallbackRequest);
 if (N<Capacity) N+=FStore::DescribeTypes(Out+N,Capacity-N);
 if (N<Capacity) N+=FCaptureQueue::DescribeTypes(Out+N,Capacity-N);
 return N;
}
} // namespace VaRest::Inspector::Private
#endif
