// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once
#include "CoreMinimal.h"
#include "VaRestMemoryCache.h"
#include "VaRestPersistenceGuard.h"
#include "VaRestPersistence.h"
namespace VaRest::Http
{
using FCacheReadDone = TUniqueFunction<void(TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe>)>;
class FDiskCache : public TSharedFromThis<FDiskCache,ESPMode::ThreadSafe>
{
public:
    static TSharedRef<FDiskCache,ESPMode::ThreadSafe> Open(FString Root, int64 Capacity);
    static void StopControls();
    /** False: executor admission refused and Done is not called. True: resolved miss or admitted read.
     * Quarantine/control exhaustion remains a conservative miss, independent of executor refusal. */
    bool Find(FString Key, double Now, FDateTime Utc, FCacheReadDone&& Done);
    void Put(FCacheEntry Entry, FPersistenceGuard Guard);
    void Remove(FString Key);
    void Clear();
    uint64 GetEpoch() const { return Epoch; }
    bool IsQuarantined() const;
private:
    void RequestMaintenance(bool bClear);
    void TryMaintenance();
    bool bMaintenancePending=true,bMaintenanceInFlight=false,bClearRequired=false;
    uint64 ControlSerial=1,WorkerAppliedControlSerial=0; // worker field is pipe-only
    FDiskCache(FString InRoot, int64 InCapacity);
    FString Directory;
    int64 CapacityBytes = 0; // GT requested monotone minimum
    int64 WorkerCapacityBytes = 0; // touched only inside the serialized pipe after construction
    uint64 Epoch = 1;
};
TArray<uint8> EncodeCacheRecord(const FCacheEntry& Entry);
TOptional<FCacheEntry> DecodeCacheRecord(TConstArrayView<uint8> Bytes, FStringView ExpectedKey,
    double Now, FDateTime Utc, int64 MaximumBodyBytes);
}

#if WITH_DEV_AUTOMATION_TESTS
namespace VaRest::Http::Testing
{
    /** Each scope owns its registries; production sticky state is never reset. */
    class FScopedCacheControls
    {
    public:
        FScopedCacheControls();
        ~FScopedCacheControls();
        FScopedCacheControls(const FScopedCacheControls&)=delete;
        FScopedCacheControls& operator=(const FScopedCacheControls&)=delete;
    private:
        void* Owned=nullptr;
    };
    struct FCacheStoreHooks
    {
        bool bFailList=false, bFailDelete=false, bFailRead=false, bDeleteBeforeRead=false;
        TOptional<EFileFault> ReadFault, WriteFault;
        TSharedPtr<FPersistenceBarrier,ESPMode::ThreadSafe> AfterRead;
    };
    class FScopedCacheStoreHooks
    {
    public:
        explicit FScopedCacheStoreHooks(FString InRoot);
        ~FScopedCacheStoreHooks();
        FScopedCacheStoreHooks(const FScopedCacheStoreHooks&)=delete;
        FScopedCacheStoreHooks& operator=(const FScopedCacheStoreHooks&)=delete;
        FString Root;
        TSharedRef<FCacheStoreHooks,ESPMode::ThreadSafe> Hooks;
    };
    int32 DirtyCacheControlsForTest();
    int32 CacheRegistryForTest();
}
#endif
