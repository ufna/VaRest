// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once
#include "CoreMinimal.h"
#include "VaRestCachePolicy.h"
#include "VaRestStage.h"
namespace VaRest::Http
{
struct FCacheEntry
{
    FGuid Generation=FGuid::NewGuid();
    FString Key; // 64 lowercase hexadecimal bytes, never request material
    FVaRestHeaders Headers;
    FString ContentType;
    TArray<uint8> Body;
    FCacheMetadata Metadata;
    uint64 LastUse = 0;
    int64 Charge() const;
    FVaRestAttemptOutcome Answer() const;
};
class FMemoryCache
{
public:
    TSharedPtr<const FCacheEntry, ESPMode::ThreadSafe> Find(const FString& Key);
    void Put(FCacheEntry Entry, int64 Capacity);
    void Remove(const FString& Key);
    void Clear();
    int64 Bytes() const { return UsedBytes; }
    int32 Num() const { return Entries.Num(); }
private:
    TMap<FString,TSharedRef<FCacheEntry,ESPMode::ThreadSafe>> Entries;
    int64 UsedBytes = 0;
    uint64 Sequence = 0;
};
}
