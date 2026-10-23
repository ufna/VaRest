// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once
#include "VaRestOfflineInternal.h"
#include "VaRestPersistenceGuard.h"
#include "Templates/ValueOrError.h"

namespace VaRest::Http
{
struct FQueueLoad
{
    TArray<FQueueItem> Items;
    uint64 NextSequence = 1;
    bool bOk = true,bPolicyExceeded=false;
    int64 PeakBodyBytes=0,PeakMetadataBytes=0;
};
int64 QueueMetadataCharge(const FQueueItem& Item);
TOptional<int64> MeasureQueueAdmissionMetadata(const FQueueItem& BodyFreeItem,FStringView Origin,
    const FVaRestOfflinePolicy& Policy);
TArray<uint8> EncodeQueueRecord(const FQueueItem& Item);
TValueOrError<FQueueItem,FString> DecodeQueueRecord(TConstArrayView<uint8> Bytes,
    FStringView ExpectedOrigin, const FVaRestOfflinePolicy& Policy,
    int64 AvailableBody=MAX_int64,bool* bCapacityRefused=nullptr);
struct FQueueCommitResult
{
    bool bCommitted=false;
    bool bCleanupOk=false; // failed/refused cleanup leaves physical occupancy; caller must fail storage closed
};
class FQueueStore
{
public:
    static void Load(FString Directory, FString Origin, FVaRestOfflinePolicy Policy,
        TUniqueFunction<void(FQueueLoad&&)>&& Done);
    static void Prepare(FString Directory, FString Origin, FVaRestOfflinePolicy Policy, FQueueItem Item, FPersistenceGuard Guard,
        TUniqueFunction<void(bool)>&& Done);
    static void Commit(FString Directory,uint64 Sequence,FString QueueId,TUniqueFunction<void(FQueueCommitResult)>&& Done);
    static void Abort(FString Directory,uint64 Sequence,FString QueueId,TUniqueFunction<void(bool)>&& Done);
    static void Retire(FString Directory, uint64 Sequence, FString QueueId,
        TUniqueFunction<void(bool)>&& Done);
};
FString QueueFileName(uint64 Sequence, FStringView QueueId);
FGuid QueueRunId(); // one random GUID per process, all jobs share it
}
