// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestMemoryCache.h"

int64 VaRest::Http::FCacheEntry::Charge() const
{
    int64 Count = 1024 + Body.Num() + (Key.Len() + ContentType.Len() + Metadata.ETag.Len() + Metadata.LastModified.Len()) * sizeof(TCHAR);
    for (const auto& Pair : Headers) Count += (Pair.Key.Len() + Pair.Value.Len()) * sizeof(TCHAR) + 64;
    for (const FString& Name : Metadata.Vary) Count += Name.Len() * sizeof(TCHAR) + 32;
    return Count;
}
FVaRestAttemptOutcome VaRest::Http::FCacheEntry::Answer() const
{
    FVaRestAttemptOutcome Result;
    Result.Status = 200;
    Result.Headers = Headers;
    Result.ContentType = ContentType;
    Result.Body = Body;
    Result.Body.Reserve(Result.Body.Num() + 1);
    Result.bFromCache = true; // added to outcome in this task
    return Result;
}
TSharedPtr<const VaRest::Http::FCacheEntry,ESPMode::ThreadSafe>
VaRest::Http::FMemoryCache::Find(const FString& Key)
{
    check(IsInGameThread());
    if (auto* Found = Entries.Find(Key)) { (*Found)->LastUse = ++Sequence; return *Found; }
    return nullptr;
}
void VaRest::Http::FMemoryCache::Remove(const FString& Key)
{
    check(IsInGameThread());
    if (const auto* Found = Entries.Find(Key)) UsedBytes -= (*Found)->Charge();
    Entries.Remove(Key);
}
void VaRest::Http::FMemoryCache::Clear() { check(IsInGameThread()); Entries.Reset(); UsedBytes = 0; }
void VaRest::Http::FMemoryCache::Put(FCacheEntry Entry, int64 Capacity)
{
    check(IsInGameThread());
    Remove(Entry.Key);
    const int64 Cost = Entry.Charge();
    if (Capacity <= 0 || Cost > Capacity) return;
    while (UsedBytes > Capacity - Cost || Entries.Num() >= 4096)
    {
        FString Oldest;
        uint64 OldestUse = MAX_uint64;
        for (const auto& Pair : Entries)
        {
            if (Pair.Value->LastUse < OldestUse) { Oldest = Pair.Key; OldestUse = Pair.Value->LastUse; }
        }
        check(!Entries.IsEmpty());
        Remove(Oldest);
    }
    Entry.LastUse = ++Sequence;
    UsedBytes += Cost;
    const FString StoredKey = Entry.Key;
    Entries.Add(StoredKey, MakeShared<FCacheEntry,ESPMode::ThreadSafe>(MoveTemp(Entry)));
}
