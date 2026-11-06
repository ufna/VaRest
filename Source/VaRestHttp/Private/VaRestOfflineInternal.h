// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestOffline.h"
#include "VaRestBody.h"
#include "VaRestAuthTypes.h"
#include "VaRestClock.h"
#include "VaRestStage.h"
#include "VaRestQueueBinding.h"
#include "VaRestConnectivity.h"
#include "VaRestQueueFence.h"

class FVaRestConnectivity;
class IVaRestAuth;
namespace VaRest::Http
{
class FClientState;
struct FClientConfig;
struct FExchangeState;
struct FQueueItem
{
    FString QueueId;
    uint64 Sequence = 0;
    FGuid OriginalRun;
    uint64 OriginalRequestId = 0;
    FDateTime CreatedUtc;
    FString Scope;
    FString Verb;
    FString Url;
    FVaRestHeaders Headers;
    FVaRestRequestBody Body;
    FString IdempotencyKey;
    FString Tag;
    FString Endpoint; // v2 exact trace metadata; v1 recovery leaves empty, FName materialized only on GT replay
    TArray<FString> OriginalPartNames; // original admission only; never encoded, recovered items leave empty
    int64 MetadataCharge=0; // ephemeral reserved max(live, canonical recovered); never serialized
    EVaRestAuthMode AuthMode = EVaRestAuthMode::Required;
    bool bIdempotent = false;
};
class FQueueAdmissionJob;
class FQueueReplayOperation;
struct FQueueAdmissionToken;
class FOfflineQueue : public TSharedFromThis<FOfflineQueue,ESPMode::ThreadSafe>
{
public:
    ~FOfflineQueue();
    static void StopAll();
    void StopForShutdown();
    static TSharedPtr<FOfflineQueue,ESPMode::ThreadSafe> Join(
        const TSharedRef<FClientState,ESPMode::ThreadSafe>& Client,FString& OutConfigError,
		TSharedPtr<FQueueBinding,ESPMode::ThreadSafe>* OutBinding=nullptr);
	/** Pure target validation, shared by ordinary Join and the atomic managed switch before any revocation. */
	static bool ValidateTarget(const FClientConfig& Config,FString& OutConfigError);
    void Leave(uint64 ClientId);
	void Leave(const TSharedPtr<FQueueBinding,ESPMode::ThreadSafe>& Binding,TSharedPtr<FNativeLifetimeControl,ESPMode::ThreadSafe> Control=nullptr);
    int32 Count() const;
	bool IsLoadedOnGT() const { check(IsInGameThread());return bLoaded && !bStopped; }
    bool Reserve(int64 BodyBytes,int64 MetadataBytes,FString& OutError);
    void ReleaseReservation(int64 BodyBytes,int64 MetadataBytes);
    void Enqueue(const FVaRestExchangeRef& Exchange,FVaRestOutcomeCallback&& Done);
    bool TryReplay(bool bProbe,FVaRestProbeToken ProbeToken={});
    bool Flush(TUniqueFunction<void(bool)>&& Done,TSharedPtr<FQueueBinding,ESPMode::ThreadSafe> OriginBinding=nullptr);
	uint64 AdmissionWatermarkOnGT() const { check(IsInGameThread());return LastAdmission; }
	void AdmitOwnedDrainOnGT() { check(IsInGameThread() && OwnedDrains<512);++OwnedDrains; }
	void AcknowledgeOwnedDrainOnGT() { check(IsInGameThread() && OwnedDrains!=0);--OwnedDrains; }
	bool MemberDrainReadyOnGT(const TSharedPtr<FQueueBinding,ESPMode::ThreadSafe>& Binding,uint64 Through) const;
	/** The caller already owns its inline fence row; no PostPersistencePoll/new waiter admission at teardown. */
	bool FlushOwned(const TSharedPtr<FQueueBinding,ESPMode::ThreadSafe>& Binding,uint64 Through,FNativeQueueFenceToken&& Token);
	bool TryFenceNamespaceOnGT(); // true also when a compatible live member keeps this coordinator live
	bool HasStorageFailureOnGT() const { check(IsInGameThread());return bStorageFailed || bFlushFailed; }
#if WITH_DEV_AUTOMATION_TESTS
    int32 NumAdmissionsForTest() const { check(IsInGameThread()); return ActiveAdmissions.Num(); }
    int32 NumFlushWaitersForTest() const { check(IsInGameThread()); return FlushWaiters.Num(); }
    int64 MetadataBytesForTest() const { check(IsInGameThread()); return CommittedMetadataBytes+ReservedMetadataBytes; }
#endif
    const FString& GetDirectory() const { return Directory; }
    void AcceptCommitted(FQueueItem Item);
    void ScheduleReplay();
private:
	enum class ECoordinatorPhase : uint8 { Initializing,Live,Draining,Fenced };
	void BeginLastMemberDrain();
	ECoordinatorPhase CoordinatorPhase=ECoordinatorPhase::Initializing;
	uint64 RegistryEpoch=0;
	uint32 OwnedDrains=0;
    void Load();
    void DrainLoadedWaiters();
    FDateTime UtcNow() const;
    TSharedPtr<FClientState,ESPMode::ThreadSafe> SelectOwner() const;
    void ReplayFinished(FString QueueId,FVaRestQueuedResult Result,bool bKeep);
    void RetireAndNotify(FString QueueId,FVaRestQueuedResult Result);
    void BroadcastLater(FVaRestQueuedResult Result);
    void FinishAdmission(uint64 Ticket);
	void AdvanceManagedReplay(const TSharedRef<FQueueReplayOperation,ESPMode::ThreadSafe>& Op);
	void FinishManagedRetire(const TSharedRef<FQueueReplayOperation,ESPMode::ThreadSafe>& Op,bool bOk);
	void ReleaseManagedReplay(const TSharedRef<FQueueReplayOperation,ESPMode::ThreadSafe>& Op,bool bNormal);
	void DiscardManagedReplay(const TSharedRef<FQueueReplayOperation,ESPMode::ThreadSafe>& Op);
    struct FFlushWaiter;
    bool PollFlush(const TSharedRef<FFlushWaiter,ESPMode::ThreadSafe>& Waiter);
    struct FFlushWaiter
    {
        uint64 Through=0;
        TUniqueFunction<void(bool)> Done;
        TOptional<bool> Result;
        bool bBarrierStarted=false;
		bool bDelivering=false;
		TSharedPtr<FQueueBinding,ESPMode::ThreadSafe> OriginBinding;
    };
    FString Directory,Origin,AuthCategory;
    FName IdentityContract;
    TWeakPtr<IVaRestAuth,ESPMode::ThreadSafe> IdentityProvider;
    FVaRestOfflinePolicy Policy;
    TWeakPtr<FVaRestConnectivity,ESPMode::ThreadSafe> Connectivity;
    FDelegateHandle ConnectivityChanged;
	struct FMember
	{
		TWeakPtr<FClientState,ESPMode::ThreadSafe> Client;
		TSharedPtr<FQueueBinding,ESPMode::ThreadSafe> Binding;
	};
	TMap<FQueueMemberKey,FMember> Members;
	uint64 NextMemberEpoch=1;
	uint64 NextReplayId=1;
	TSharedPtr<FQueueReplayOperation,ESPMode::ThreadSafe> ManagedReplay;
	TSharedRef<FQueueCaptureGate,ESPMode::ThreadSafe> CaptureGate=MakeShared<FQueueCaptureGate,ESPMode::ThreadSafe>();
	TSharedRef<std::atomic<uint32>,ESPMode::ThreadSafe> OwnedFlushes=MakeShared<std::atomic<uint32>,ESPMode::ThreadSafe>(0);
    TSharedPtr<IVaRestClock,ESPMode::ThreadSafe> QueueClock;
    FDateTime UtcOrigin;
    double MonotonicOrigin=0;
    int32 ReservedCount=0;
    int64 ReservedBodyBytes=0,CommittedBodyBytes=0,WaitingBodyBytes=0;
    int64 ReservedMetadataBytes=0,CommittedMetadataBytes=0;
    TArray<FQueueItem> Items;
    TArray<TUniqueFunction<void()>> AfterLoad;
    uint64 NextSequence=1,LastAdmission=0;
	// Only live tickets, with the exact originating registration. Detached A never waits for B's later admissions.
	TMap<uint64,FQueueMemberKey> ActiveAdmissions;
    TArray<TSharedRef<FFlushWaiter,ESPMode::ThreadSafe>> FlushWaiters;
    bool bStopped=false;
	bool bStopCleanupActive=false;
    FVaRestTimerHandle ReplayRetryTimer;
    double ReplayNotBefore=0;
    bool bLoaded=false,bReplayActive=false,bStorageFailed=false,bReplayScheduled=false,bProbeReplay=false,bFlushFailed=false;
	FQueueMemberKey ReplayOwner;
	FVaRestProbeToken ReplayProbeToken;
    friend class FQueueAdmissionJob;
    friend struct FQueueAdmissionToken;
	friend class FQueueReplayOperation;
};
FVaRestStageRef MakeOfflineStage();
FString QueueDirectory(FStringView Root, FStringView Name);
bool QueueScope(const FExchangeState& Ex, FString& OutScope);
bool QueueIdentityStillCurrent(const FExchangeState& Ex);
bool SameQueueRegistrationTarget(const FClientConfig& A,const FClientConfig& B);

FVaRestAttemptOutcome LocalFailure(FName Detail);
TArray<uint8> CopyQueueAdmissionBytes(TConstArrayView<uint8> Bytes);
}
