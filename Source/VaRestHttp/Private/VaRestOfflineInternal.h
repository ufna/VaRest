// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestOffline.h"
#include "VaRestBody.h"
#include "VaRestAuthTypes.h"
#include "VaRestClock.h"
#include "VaRestStage.h"

class FVaRestConnectivity;
class IVaRestAuth;
namespace VaRest::Http
{
class FClientState;
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
    TArray<FString> OriginalPartNames; // original admission only; never encoded, recovered items leave empty
    int64 MetadataCharge=0; // ephemeral reserved max(live, canonical recovered); never serialized
    EVaRestAuthMode AuthMode = EVaRestAuthMode::Required;
    bool bIdempotent = false;
};
class FQueueAdmissionJob;
struct FQueueAdmissionToken;
class FOfflineQueue : public TSharedFromThis<FOfflineQueue,ESPMode::ThreadSafe>
{
public:
    ~FOfflineQueue();
    static void StopAll();
    void StopForShutdown();
    static TSharedPtr<FOfflineQueue,ESPMode::ThreadSafe> Join(
        const TSharedRef<FClientState,ESPMode::ThreadSafe>& Client,FString& OutConfigError);
    void Leave(uint64 ClientId);
    int32 Count() const;
    bool Reserve(int64 BodyBytes,int64 MetadataBytes,FString& OutError);
    void ReleaseReservation(int64 BodyBytes,int64 MetadataBytes);
    void Enqueue(const FVaRestExchangeRef& Exchange,FVaRestOutcomeCallback&& Done);
    bool TryReplay(bool bProbe);
    bool Flush(TUniqueFunction<void(bool)>&& Done);
#if WITH_DEV_AUTOMATION_TESTS
    int32 NumFlushWaitersForTest() const { check(IsInGameThread()); return FlushWaiters.Num(); }
    int64 MetadataBytesForTest() const { check(IsInGameThread()); return CommittedMetadataBytes+ReservedMetadataBytes; }
#endif
    const FString& GetDirectory() const { return Directory; }
    void AcceptCommitted(FQueueItem Item);
    void ScheduleReplay();
private:
    void Load();
    void DrainLoadedWaiters();
    FDateTime UtcNow() const;
    TSharedPtr<FClientState,ESPMode::ThreadSafe> SelectOwner() const;
    void ReplayFinished(FString QueueId,FVaRestQueuedResult Result,bool bKeep);
    void RetireAndNotify(FString QueueId,FVaRestQueuedResult Result);
    void BroadcastLater(FVaRestQueuedResult Result);
    void FinishAdmission(uint64 Ticket);
    struct FFlushWaiter;
    bool PollFlush(const TSharedRef<FFlushWaiter,ESPMode::ThreadSafe>& Waiter);
    struct FFlushWaiter
    {
        uint64 Through=0;
        TUniqueFunction<void(bool)> Done;
        TOptional<bool> Result;
        bool bBarrierStarted=false;
    };
    FString Directory,Origin,AuthCategory;
    FName IdentityContract;
    TWeakPtr<IVaRestAuth,ESPMode::ThreadSafe> IdentityProvider;
    FVaRestOfflinePolicy Policy;
    TWeakPtr<FVaRestConnectivity,ESPMode::ThreadSafe> Connectivity;
    FDelegateHandle ConnectivityChanged;
    TMap<uint64,TWeakPtr<FClientState,ESPMode::ThreadSafe>> Members;
    TSharedPtr<IVaRestClock,ESPMode::ThreadSafe> QueueClock;
    FDateTime UtcOrigin;
    double MonotonicOrigin=0;
    int32 ReservedCount=0;
    int64 ReservedBodyBytes=0,CommittedBodyBytes=0,WaitingBodyBytes=0;
    int64 ReservedMetadataBytes=0,CommittedMetadataBytes=0;
    TArray<FQueueItem> Items;
    TArray<TUniqueFunction<void()>> AfterLoad;
    uint64 NextSequence=1,LastAdmission=0;
    TSet<uint64> ActiveAdmissions;
    TArray<TSharedRef<FFlushWaiter,ESPMode::ThreadSafe>> FlushWaiters;
    bool bStopped=false;
    FVaRestTimerHandle ReplayRetryTimer;
    double ReplayNotBefore=0;
    bool bLoaded=false,bReplayActive=false,bStorageFailed=false,bReplayScheduled=false,bProbeReplay=false,bFlushFailed=false;
    friend class FQueueAdmissionJob;
    friend struct FQueueAdmissionToken;
};
FVaRestStageRef MakeOfflineStage();
FString QueueDirectory(FStringView Root, FStringView Name);
bool QueueScope(const FExchangeState& Ex, FString& OutScope);
bool QueueIdentityStillCurrent(const FExchangeState& Ex);

FVaRestAttemptOutcome LocalFailure(FName Detail);
TArray<uint8> CopyQueueAdmissionBytes(TConstArrayView<uint8> Bytes);
}
