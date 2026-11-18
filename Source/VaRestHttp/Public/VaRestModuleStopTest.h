// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

// This opt-in test contract imports ONLY Core. The ordinary smoke witness includes this header
// textually; it must never link VaRestHttp, CoreUObject or Engine. No product links the smoke module.
#include "CoreMinimal.h"
#include "Features/IModularFeature.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace VaRest::ModuleStopTest
{
inline FName FeatureName() { return FName(TEXT("VaRest.M5.ModuleStopWitness.v1")); }
enum class ERoute : uint8 { Ordinary, Managed };
enum class EPublication : uint8
{
 OrdinaryReady, ManagedWorkerEnded, ManagedAccessEnded,
 ManagedReturnExecuted, ManagedReturnCompleted, ManagedReturnCaptureRan, ManagedReturnCaptureAbandoned
};
struct FWorkerIdentity
{
 ERoute Route = ERoute::Ordinary;
 uint64 Request = 0, Client = 0, Generation = 0, CellGeneration = 0;
 uint16 Cell = 0;
 uint64 NativeToken = 0; // actual FNativeDynamicWorker/FManagedCompletionWorker allocation identity, captured on GT
};
struct FGTObservation
{
 bool FramesStopped = false, OwnedReflectionRetired = false, GateClosed = false;
 bool ControlStopped = false, ExitRetention = false, AllNativeOperationsOnGT = false;
 bool TraceMembershipMatches = false, TraceMaskMatches = false;
 int32 TraceSinkCount = -1;
 uint32 TraceMask = 0;
 int32 Carriers = -1, CarrierBaseline = -1;
 uint32 OccupiedClients = 0, OccupiedCells = 0, CountedOperations = 0;
 uint64 PendingInputs = 0;
 int64 OrdinaryClockWorkerCalls = -1, ManagedClockWorkerCalls = -1;
};
/** All implementations and data behind this interface are plain owned native state.
 * Worker methods accept values only. No reflection, callback, clock, module/service lookup or GT handback.
 */
class INativeProbe
{
public:
 virtual ~INativeProbe() = default;
 virtual void HoldAfterNativeParse(FWorkerIdentity Id, bool Parsed, bool TimeDecode, uint64 ClockAddress, uint32 Thread, bool OnGT) = 0;
 virtual void Published(FWorkerIdentity Id, EPublication Event, uint32 Thread, bool OnGT, bool Stopped) = 0;
 virtual void ReflectedMaterializationOnGT() = 0;
 virtual void UserDeliveryOnGT() = 0;
 virtual bool BothHeldOnGT() const = 0;
 virtual bool FailedOnGT() const = 0;
 virtual bool ArmOnGT() = 0; // ARMING only; never a lifecycle pass
 virtual void SetupOnGT(bool OptionalUnknownRefused, bool OptionalLeafRefused,
   bool MaterializedAndTaken, bool ShellDestroyedOffGT) = 0;
 virtual bool ObserveClockOnGT(bool Timing, uint32 TraceMask, uint64 OrdinaryClock, uint64 ManagedClock,
   int64 OrdinaryGT, int64 OrdinaryWorker, int64 ManagedGT, int64 ManagedWorker) = 0;
 virtual void FailOnGT() = 0;
};
class IWitness : public IModularFeature
{
public:
 virtual bool StartupPrecededHttpOnGT() const = 0;
 virtual TSharedPtr<INativeProbe, ESPMode::ThreadSafe> CreateProbeOnGT(const FString& Case) = 0;
 // Kept separately from the native probe. Real OnPreExit invokes then destroys it ON GT,
 // before any post-module-release phase. No worker ever retains this callable.
 virtual bool SetGTObserverOnGT(TUniqueFunction<FGTObservation()>&& Observer) = 0;
};
}
#endif
