// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestDiskCache.h"
#include "VaRestJson.h"
#include "Misc/Base64.h"
#include "Misc/Crc.h"
#include "Misc/Paths.h"
#include "VaRestSha256.h"
#include "VaRestUrl.h"
#include "HAL/PlatformFileManager.h"

namespace VaRest::Http::DiskCachePrivate
{
    // FString's default equality/hash fold case. Linux directory names do not.
    // Other platforms retain their existing semantics pending native alias checks.
    template<typename ValueType>
    struct FRootKeyFuncs : TDefaultMapHashableKeyFuncs<FString,ValueType,false>
    {
        static bool Matches(const FString& Left,const FString& Right)
        {
#if PLATFORM_LINUX
            return Left.Equals(Right,ESearchCase::CaseSensitive);
#else
            return Left==Right;
#endif
        }
        static uint32 GetKeyHash(const FString& Key)
        {
#if PLATFORM_LINUX
            return FCrc::StrCrc32(*Key);
#else
            return GetTypeHash(Key);
#endif
        }
    };
    template<typename ValueType>
    using TRootMap=TMap<FString,ValueType,FDefaultSetAllocator,FRootKeyFuncs<ValueType>>;

    // Separate logical workspace accounts, not an allocator/RSS promise. With a 12 MiB
    // encoded read ceiling, 8x record + 16 MiB index/metadata slack is at most 112 MiB.
    // Put adds the guard's body/JSON workspace; the executor may refuse that candidate.
    constexpr int64 MaxBodyBytes=32LL*1024*1024;
    constexpr int64 MaxRecordBytes=12LL*1024*1024;
    constexpr int64 WorkspaceBytes=16LL*1024*1024;
    constexpr int64 MetadataEncodedBytes=512LL*1024;
    constexpr int32 MaxRecords=4096;

    bool BoundedSyntax(TConstArrayView<uint8> Bytes)
    {
        // Gate tiny-node floods before allocating a DOM. Strings are byte-scanned,
        // not copied; the JSON parser remains the syntax/UTF-8 authority.
        int32 Units=0,Depth=0;
        bool bString=false,bEscape=false,bAtom=false;
        for (uint8 Byte : Bytes)
        {
            if (bString)
            {
                if (bEscape) bEscape=false;
                else if (Byte=='\\') bEscape=true;
                else if (Byte=='"') bString=false;
                continue;
            }
            if (Byte=='"') { bString=true; bAtom=false; ++Units; }
            else if (Byte=='{' || Byte=='[') { if (++Depth>4) return false; ++Units; bAtom=false; }
            else if (Byte=='}' || Byte==']') { if (--Depth<0) return false; bAtom=false; }
            else if (Byte==',' || Byte==':' || Byte==' ' || Byte=='\n' || Byte=='\r' || Byte=='\t') bAtom=false;
            else if (!bAtom) { bAtom=true; ++Units; }
            if (Units>2048) return false;
        }
        return !bString && Depth==0;
    }
    bool LowerHex(FStringView Text,int32 Length)
    {
        if (Text.Len()!=Length) return false;
        for (TCHAR Character : Text)
            if (!((Character>=TEXT('0') && Character<=TEXT('9')) || (Character>=TEXT('a') && Character<=TEXT('f')))) return false;
        return true;
    }
    bool HeadersFromJson(const FVaRestJson& Array,FVaRestHeaders& Out)
    {
        if (!Array.IsArray() || Array.Num()>128) return false;
        int64 Characters=0;
        const FVaRestRedactRules Rules;
        for (const auto& Pair : Array.Elements())
        {
            if (!Pair.IsArray() || Pair.Num()!=2 || !Pair[0].IsString() || !Pair[1].IsString()) return false;
            if (Pair[0].AsUtf8View().Len()+Pair[1].AsUtf8View().Len()>262144) return false;
            const FString Name=Pair[0].AsString(),Value=Pair[1].AsString();
            Characters+=Name.Len()+Value.Len();
            if (Characters>65536 || !IsValidHeaderName(Name) || !IsValidHeaderValue(Value)
                || Rules.ShouldRedact(Name,EVaRestRedactKeyKind::Header)) return false;
            Out.Add(Name,Value);
        }
        return FPersistenceGuard().SafeHeaders(Out).Num()==Out.Num();
    }
}
TArray<uint8> VaRest::Http::EncodeCacheRecord(const FCacheEntry& Entry)
{
    FVaRestJson Root=FVaRestJson::Object();
    Root.Set(TEXT("version"),1);
    Root.Set(TEXT("key"),Entry.Key);
    Root.Set(TEXT("generation"),Entry.Generation.ToString(EGuidFormats::DigitsLower));
    Root.Set(TEXT("storedUtc"),Entry.Metadata.StoredUtc.ToIso8601());
    Root.Set(TEXT("usedUtc"),Entry.Metadata.StoredUtc.ToIso8601());
    Root.Set(TEXT("lifetime"),Entry.Metadata.Lifetime);
    Root.Set(TEXT("initialAge"),Entry.Metadata.InitialAge);
    Root.Set(TEXT("noCache"),Entry.Metadata.bNoCache);
    Root.Set(TEXT("mustRevalidate"),Entry.Metadata.bMustRevalidate);
    Root.Set(TEXT("contentType"),Entry.ContentType);
    Root.Set(TEXT("body"),FBase64::Encode(Entry.Body));
    Root.Set(TEXT("bodySha256"),FSha256::Hash(Entry.Body).ToHex());
    FVaRestJson Headers=FVaRestJson::Array();
    for (const auto& Pair : Entry.Headers) Headers.Add(FVaRestJson::Array(Pair.Key,Pair.Value));
    Root.Set(TEXT("headers"),Headers);
    TArray<uint8> Bytes;
    const auto Written=Root.TryToUtf8(Bytes);
    return Written.HasValue() ? MoveTemp(Bytes) : TArray<uint8>();
}
TOptional<VaRest::Http::FCacheEntry> VaRest::Http::DecodeCacheRecord(TConstArrayView<uint8> Bytes,
    FStringView ExpectedKey,double Now,FDateTime Utc,int64 MaximumBodyBytes)
{
    using namespace DiskCachePrivate;
    MaximumBodyBytes=FMath::Clamp<int64>(MaximumBodyBytes,0,MaxBodyBytes);
    if (!FMath::IsFinite(Now) || Bytes.Num()>MaxRecordBytes || !BoundedSyntax(Bytes)) return {};
    FVaRestJsonParseOptions Options;
    Options.MaxDepth=4;
    Options.MaxInputBytes=FMath::Min<int64>(MaxRecordBytes,((MaximumBodyBytes+2)/3)*4+MetadataEncodedBytes);
    auto Parsed=FVaRestJson::Parse(Bytes,Options);
    if (Parsed.HasError()) return {};
    const FVaRestJson Root=Parsed.StealValue();
    if (!Root.IsObject() || Root.Num()!=13 || !Root[TEXT("version")].IsNumber() || Root[TEXT("version")].AsDouble()!=1.0) return {};
    for (const TCHAR* Name : {TEXT("key"),TEXT("generation"),TEXT("storedUtc"),TEXT("usedUtc"),
        TEXT("contentType"),TEXT("body"),TEXT("bodySha256")})
        if (!Root[Name].IsString()) return {};
    if (!Root[TEXT("noCache")].IsBool() || !Root[TEXT("mustRevalidate")].IsBool()) return {};
    for (const TCHAR* Name : {TEXT("key"),TEXT("generation"),TEXT("storedUtc"),TEXT("usedUtc"),TEXT("bodySha256")})
        if (Root[Name].AsUtf8View().Len()>64) return {};
    if (Root[TEXT("contentType")].AsUtf8View().Len()>4096) return {};
    if (!Root[TEXT("lifetime")].IsNumber() || !Root[TEXT("initialAge")].IsNumber()) return {};
    const auto Lifetime=Root[TEXT("lifetime")].TryGet<double>();
    const auto Age=Root[TEXT("initialAge")].TryGet<double>();
    if (!Lifetime.IsSet() || !Age.IsSet() || !FMath::IsFinite(*Lifetime) || !FMath::IsFinite(*Age)
        || *Lifetime<0 || *Lifetime>315360000 || *Age<0 || *Age>315360000) return {};
    FCacheEntry Entry;
    Entry.Key=Root[TEXT("key")].AsString();
    if (!DiskCachePrivate::LowerHex(Entry.Key,64) || Entry.Key!=ExpectedKey) return {};
    const FString GenerationText=Root[TEXT("generation")].AsString();
    if (!DiskCachePrivate::LowerHex(GenerationText,32) || !FGuid::ParseExact(GenerationText,EGuidFormats::Digits,Entry.Generation) || !Entry.Generation.IsValid()) return {};
    FDateTime Stored,Used;
    if (!FDateTime::ParseIso8601(*Root[TEXT("storedUtc")].AsString(),Stored)
        || !FDateTime::ParseIso8601(*Root[TEXT("usedUtc")].AsString(),Used)) return {};
    Entry.ContentType=Root[TEXT("contentType")].AsString();
    if (Entry.ContentType.Len()>1024 || !IsValidHeaderValue(Entry.ContentType)
        || !DiskCachePrivate::HeadersFromJson(Root[TEXT("headers")],Entry.Headers)) return {};
    const auto Base64=Root[TEXT("body")].AsUtf8View();
    if (Base64.Len()%4!=0 || Base64.Len()>((MaximumBodyBytes+2)/3)*4) return {};
    int32 Padding=0;
    for (int32 Index=0;Index<Base64.Len();++Index)
    {
        const uint8 Character=static_cast<uint8>(Base64[Index]);
        if (Character=='=') { if (Index<Base64.Len()-2 || ++Padding>2) return {}; }
        else if (Padding || !((Character>='A' && Character<='Z') || (Character>='a' && Character<='z')
            || (Character>='0' && Character<='9') || Character=='+' || Character=='/')) return {};
    }
    const int64 DecodedBytes=(Base64.Len()/4LL)*3-Padding;
    if (DecodedBytes>MaximumBodyBytes) return {};
    Entry.Body.SetNumUninitialized(static_cast<int32>(DecodedBytes));
    if (DecodedBytes && !FBase64::Decode(reinterpret_cast<const ANSICHAR*>(Base64.GetData()),Base64.Len(),Entry.Body.GetData())) return {};
    if (!FSha256::Hash(Entry.Body).ToHex().Equals(Root[TEXT("bodySha256")].AsString(),ESearchCase::CaseSensitive)) return {};
    Entry.Metadata=ReadCacheMetadata(Entry.Headers,0,Now,Utc);
    if (Entry.Metadata.bNoStore) return {};
    Entry.Metadata.Lifetime=Stored>Utc ? 0.0 : *Lifetime;
    Entry.Metadata.InitialAge=*Age+FMath::Max(0.0,(Utc-Stored).GetTotalSeconds());
    Entry.Metadata.bNoCache|=Root[TEXT("noCache")].AsBool();
    Entry.Metadata.bMustRevalidate|=Root[TEXT("mustRevalidate")].AsBool();
    Entry.Metadata.StoredAt=Now;
    Entry.Metadata.StoredUtc=Utc;
    return MoveTemp(Entry);
}

namespace VaRest::Http::DiskCachePrivate
{
#if WITH_DEV_AUTOMATION_TESTS
    using FHooks=TSharedPtr<const Testing::FCacheStoreHooks,ESPMode::ThreadSafe>;
    TRootMap<TWeakPtr<Testing::FCacheStoreHooks,ESPMode::ThreadSafe>>& HookRegistry()
    { static auto* Value=new TRootMap<TWeakPtr<Testing::FCacheStoreHooks,ESPMode::ThreadSafe>>(); return *Value; }
    FHooks CaptureHooks(const FString& Root)
    {
        check(IsInGameThread());
        if (auto Value=HookRegistry().FindRef(Root).Pin()) return MakeShared<Testing::FCacheStoreHooks,ESPMode::ThreadSafe>(*Value);
        return nullptr;
    }
#else
    struct FHooks {};
    FHooks CaptureHooks(const FString&) { return {}; }
#endif
    FIoResult Failure(const TCHAR* Detail)
    { FIoResult Result; Result.Detail=Detail; return Result; }
    FIoResult List(const FString& Root,const FHooks& Hooks)
    {
        check(!IsInGameThread());
#if WITH_DEV_AUTOMATION_TESTS
        if (Hooks.IsValid() && Hooks->bFailList) return Failure(TEXT("CacheList"));
#endif
        return ListFiles(Root);
    }
    FIoResult Delete(const FString& Path,const FHooks& Hooks)
    {
        check(!IsInGameThread());
#if WITH_DEV_AUTOMATION_TESTS
        if (Hooks.IsValid() && Hooks->bFailDelete) return Failure(TEXT("CacheDelete"));
#endif
        if (IsKnownSymlink(Path))
            return Failure(TEXT("Symlink"));
        return VaRest::Http::DeleteFile(Path);
    }
    bool RecordName(const FString& Name)
    {
        return Name.Len()==102 && Name[64]==TEXT('.') && Name.EndsWith(TEXT(".json"),ESearchCase::CaseSensitive)
            && LowerHex(FStringView(*Name,64),64) && LowerHex(FStringView(*Name+65,32),32);
    }
    struct FRecord { FString Name; int64 Size=0; FDateTime Accessed; };
    struct FScan { FIoResult Status; TArray<FRecord> Records; };
    FScan Scan(const FString& Root,const FHooks& Hooks)
    {
        check(!IsInGameThread());
        FScan Result;
        Result.Status=List(Root,Hooks);
        if (!Result.Status.bOk) return Result;
        IPlatformFile& Platform=FPlatformFileManager::Get().GetPlatformFile();
        for (const FString& Name : Result.Status.Names)
        {
            const FString Path=FPaths::Combine(Root,Name);
            if (Name.EndsWith(TEXT(".part")))
            {
                if (!Delete(Path,Hooks).bOk) { Result.Status=Failure(TEXT("PartCleanup")); return Result; }
                continue;
            }
            if (!RecordName(Name)) continue;
            const auto Stat=Platform.GetStatData(*Path);
            if (!Stat.bIsValid)
            {
                if (Platform.FileExists(*Path)) { Result.Status=Failure(TEXT("Stat")); return Result; }
                continue; // A file removed between listing/stat contributes no retained bytes.
            }
            if (Stat.bIsDirectory || IsKnownSymlink(Path)) continue;
            FRecord Record; Record.Name=Name; Record.Size=Stat.FileSize; Record.Accessed=Stat.ModificationTime;
            if (Record.Size<0) { Result.Status=Failure(TEXT("Stat")); return Result; }
            Result.Records.Add(MoveTemp(Record));
        }
        Result.Status.Names.Reset();
        Result.Records.Sort([](const FRecord& Left,const FRecord& Right)
        { return Left.Accessed==Right.Accessed ? Left.Name<Right.Name : Left.Accessed<Right.Accessed; });
        return Result;
    }
    bool Trim(const FString& Root,TArray<FRecord>& Records,int64 Capacity,int64 Incoming,const FHooks& Hooks)
    {
        int64 Bytes=0;
        for (const auto& Record : Records)
        {
            if (Record.Size>MAX_int64-Bytes) return false;
            Bytes+=Record.Size;
        }
        const int32 Limit=MaxRecords-(Incoming>0 ? 1 : 0);
        while (!Records.IsEmpty() && (Bytes>Capacity-Incoming || Records.Num()>Limit))
        {
            if (!Delete(FPaths::Combine(Root,Records[0].Name),Hooks).bOk) return false;
            Bytes-=Records[0].Size; Records.RemoveAt(0);
        }
        return Bytes<=Capacity-Incoming && Records.Num()<=Limit;
    }
    FIoResult Maintain(const FString& Root,int64 Capacity,bool bClear,const FHooks& Hooks)
    {
        FScan Scanned=Scan(Root,Hooks);
        if (!Scanned.Status.bOk) return MoveTemp(Scanned.Status);
        if (bClear)
        {
            for (const auto& Record : Scanned.Records)
                if (!Delete(FPaths::Combine(Root,Record.Name),Hooks).bOk) return Failure(TEXT("Clear"));
        }
        else if (!Trim(Root,Scanned.Records,Capacity,0,Hooks)) return Failure(TEXT("Trim"));
        return MoveTemp(Scanned.Status);
    }
    TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Read(const FString& Root,const FString& Key,
        double Now,FDateTime Utc,int64 Capacity,int64 RetainedCapacity,const FHooks& Hooks)
    {
        FScan Scanned=Scan(Root,Hooks);
        if (!Scanned.Status.bOk || !Trim(Root,Scanned.Records,RetainedCapacity,0,Hooks)) return nullptr;
        // Capacity is the immutable read/workspace limit, not the namespace retention limit.
        for (int32 Index=Scanned.Records.Num()-1;Index>=0;--Index)
        {
            const auto& Record=Scanned.Records[Index];
            if (FStringView(*Record.Name,64)!=Key) continue;
            const FString Path=FPaths::Combine(Root,Record.Name);
            FIoResult Loaded;
#if WITH_DEV_AUTOMATION_TESTS
            if (Hooks.IsValid() && Hooks->bDeleteBeforeRead) Delete(Path,Hooks);
            if (Hooks.IsValid() && Hooks->bFailRead) Loaded=Failure(TEXT("CacheRead"));
            else if (Hooks.IsValid() && Hooks->ReadFault.IsSet()) Loaded=Testing::ReadWithFault(Path,Capacity,*Hooks->ReadFault);
            else
#endif
                Loaded=ReadBoundedFile(Path,Capacity);
            auto Entry=Loaded.bOk ? DecodeCacheRecord(Loaded.Bytes,Key,Now,Utc,FMath::Min(Capacity,MaxBodyBytes)) : TOptional<FCacheEntry>();
            if (!Entry.IsSet() || Record.Name!=Key+TEXT(".")+Entry->Generation.ToString(EGuidFormats::DigitsLower)+TEXT(".json"))
            { Delete(Path,Hooks); continue; }
            FPlatformFileManager::Get().GetPlatformFile().SetTimeStamp(*Path,Utc);
            for (const auto& Other : Scanned.Records)
                if (FStringView(*Other.Name,64)==Key && Other.Name!=Record.Name) Delete(FPaths::Combine(Root,Other.Name),Hooks);
#if WITH_DEV_AUTOMATION_TESTS
            if (Hooks.IsValid()) Testing::WaitPersistenceBarrier(Hooks->AfterRead);
#endif
            return MakeShared<FCacheEntry,ESPMode::ThreadSafe>(MoveTemp(*Entry));
        }
        return nullptr;
    }
    struct FControls
    {
        TRootMap<TWeakPtr<FDiskCache,ESPMode::ThreadSafe>> Registry;
        TRootMap<TSharedPtr<FDiskCache,ESPMode::ThreadSafe>> Dirty;
        bool bExhausted=false;
    };
#if WITH_DEV_AUTOMATION_TESTS
    FControls* TestControls=nullptr;
#endif
    FControls& Controls()
    {
        check(IsInGameThread());
#if WITH_DEV_AUTOMATION_TESTS
        if (TestControls) return *TestControls;
#endif
        static auto* Instance=new FControls(); return *Instance;
    }
    bool RetainIntent(const FString& Root,const TSharedRef<FDiskCache,ESPMode::ThreadSafe>& Store)
    {
        auto& State=Controls();
        if (State.bExhausted) return false;
        // Three root copies per dirty intent, at most 4096 * 3 * 8192 * sizeof(TCHAR)
        // (384 MiB for 4-byte TCHAR), plus bounded store/map entries. Separate from IO DTOs.
        if (Root.Len()>4096 || Root.GetAllocatedSize()>8192*sizeof(TCHAR)
            || (!State.Dirty.Contains(Root) && State.Dirty.Num()>=4096))
        { State.bExhausted=true; return false; }
        State.Dirty.Add(Root,Store); return true;
    }
    bool EntryBounds(const FCacheEntry& Entry)
    {
        if (!LowerHex(Entry.Key,64) || Entry.ContentType.Len()>1024 || Entry.Body.Num()>MaxBodyBytes
            || Entry.Metadata.bNoStore || !Entry.Generation.IsValid() || !FMath::IsFinite(Entry.Metadata.Lifetime)
            || !FMath::IsFinite(Entry.Metadata.InitialAge) || Entry.Metadata.Lifetime<0 || Entry.Metadata.Lifetime>315360000
            || Entry.Metadata.InitialAge<0 || Entry.Metadata.InitialAge>315360000 || !IsValidHeaderValue(Entry.ContentType)) return false;
        int64 Characters=0; int32 Count=0;
        for (const auto& Pair : Entry.Headers)
        {
            Characters+=int64(Pair.Key.Len())+Pair.Value.Len();
            if (++Count>128 || Characters>65536 || !IsValidHeaderName(Pair.Key) || !IsValidHeaderValue(Pair.Value)) return false;
        }
        return true;
    }
}

void VaRest::Http::FDiskCache::StopControls()
{
    check(IsInGameThread()); check(!PersistenceAccepting()); check(PersistenceIdle());
    auto& Controls=DiskCachePrivate::Controls();
    Controls.bExhausted=true;
    Controls.Dirty.Reset(); Controls.Registry.Reset();
}
VaRest::Http::FDiskCache::FDiskCache(FString InRoot,int64 InCapacity)
    : Directory(MoveTemp(InRoot)),CapacityBytes(InCapacity),WorkerCapacityBytes(InCapacity) {}
bool VaRest::Http::FDiskCache::IsQuarantined() const
{ check(IsInGameThread()); return bMaintenancePending || DiskCachePrivate::Controls().bExhausted; }
void VaRest::Http::FDiskCache::RequestMaintenance(bool bClear)
{
    check(IsInGameThread());
    ++Epoch; ++ControlSerial; bMaintenancePending=true; bClearRequired|=bClear;
    DiskCachePrivate::RetainIntent(Directory,AsShared());
    TryMaintenance();
}
void VaRest::Http::FDiskCache::TryMaintenance()
{
    check(IsInGameThread());
    if (!bMaintenancePending || bMaintenanceInFlight || DiskCachePrivate::Controls().bExhausted || !PersistenceAccepting()) return;
    if (!DiskCachePrivate::RetainIntent(Directory,AsShared())) return;
    const uint64 Requested=ControlSerial;
    const int64 Desired=CapacityBytes;
    const bool bClear=bClearRequired;
    const auto StoreHooks=DiskCachePrivate::CaptureHooks(Directory);
#if WITH_DEV_AUTOMATION_TESTS
    const auto Hooks=Testing::PersistenceHooksFor(Directory);
#endif
    bMaintenanceInFlight=true;
    const bool bAccepted=LaunchPersistence([Self=AsShared(),Requested,Desired,bClear,StoreHooks
#if WITH_DEV_AUTOMATION_TESTS
        ,Hooks
#endif
    ]
    {
#if WITH_DEV_AUTOMATION_TESTS
        if (Hooks.IsValid() && Hooks->bFailCacheControl) return DiskCachePrivate::Failure(TEXT("CacheControl"));
#endif
        FIoResult Result=DiskCachePrivate::Maintain(Self->Directory,Desired,bClear,StoreHooks);
        if (Result.bOk)
        { Self->WorkerCapacityBytes=FMath::Min(Self->WorkerCapacityBytes,Desired); Self->WorkerAppliedControlSerial=Requested; }
        return Result;
    },[Self=AsShared(),Requested](FIoResult&& Result)
    {
        Self->bMaintenanceInFlight=false;
        if (Result.bOk && Self->ControlSerial==Requested)
        {
            Self->bMaintenancePending=false; Self->bClearRequired=false;
            DiskCachePrivate::Controls().Dirty.Remove(Self->Directory);
        }
        // A later access retries. Failure/refusal never recursively spins or drops intent.
    },DiskCachePrivate::WorkspaceBytes);
    if (!bAccepted) bMaintenanceInFlight=false;
}
TSharedRef<VaRest::Http::FDiskCache,ESPMode::ThreadSafe> VaRest::Http::FDiskCache::Open(FString Root,int64 Capacity)
{
    check(IsInGameThread());
    FPaths::NormalizeDirectoryName(Root);
    Root=FPaths::ConvertRelativePathToFull(Root);
    const bool bCanonical=FPaths::CollapseRelativeDirectories(Root);
    auto& Controls=DiskCachePrivate::Controls();
    auto& Values=Controls.Registry;
    for (auto It=Values.CreateIterator();It;++It) if (!It.Value().IsValid()) It.RemoveCurrent();
    // Do not retain oversized roots even in the weak registry after fallback.
    if (!bCanonical || Root.Len()>4096 || Root.GetAllocatedSize()>8192*sizeof(TCHAR)) Controls.bExhausted=true;
    Capacity=FMath::Max<int64>(0,Capacity);
    if (!Controls.bExhausted)
        if (auto Existing=Values.FindRef(Root).Pin())
        {
            if (Capacity<Existing->CapacityBytes)
            { Existing->CapacityBytes=Capacity; Existing->RequestMaintenance(false); }
            else Existing->TryMaintenance();
            return Existing.ToSharedRef();
        }
    TSharedRef<FDiskCache,ESPMode::ThreadSafe> Shared=MakeShareable(new FDiskCache(Controls.bExhausted ? FString() : Root,Capacity));
    if (!Controls.bExhausted) { Values.Add(Root,Shared); Shared->TryMaintenance(); }
    return Shared;
}
bool VaRest::Http::FDiskCache::Find(FString Key,double Now,FDateTime Utc,FCacheReadDone&& Done)
{
    check(IsInGameThread());
    if (IsQuarantined()) TryMaintenance();
    if (!DiskCachePrivate::LowerHex(Key,64) || DiskCachePrivate::Controls().bExhausted
        || (bMaintenancePending && !bMaintenanceInFlight)) { Done(nullptr); return true; }
    const uint64 RequiredControl=ControlSerial;
    const uint64 ExpectedEpoch=Epoch;
    const int64 ReadCapacity=FMath::Min(CapacityBytes,DiskCachePrivate::MaxRecordBytes);
    const TWeakPtr<FDiskCache,ESPMode::ThreadSafe> Weak(AsShared());
    const auto StoreHooks=DiskCachePrivate::CaptureHooks(Directory);
#if WITH_DEV_AUTOMATION_TESTS
    const auto Hooks=Testing::PersistenceHooksFor(Directory);
#endif
    auto Callback=MakeShared<FCacheReadDone,ESPMode::ThreadSafe>(MoveTemp(Done));
    const bool bAccepted=LaunchPersistenceResult<TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe>>(
        [Store=AsShared(),Key=MoveTemp(Key),Now,Utc,ReadCapacity,RequiredControl,StoreHooks
#if WITH_DEV_AUTOMATION_TESTS
            ,Hooks
#endif
        ]
        {
#if WITH_DEV_AUTOMATION_TESTS
            if (Hooks.IsValid()) Testing::WaitPersistenceBarrier(Hooks->BeforeRead);
#endif
            if (Store->WorkerAppliedControlSerial!=RequiredControl) return TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe>();
            return DiskCachePrivate::Read(Store->Directory,Key,Now,Utc,ReadCapacity,Store->WorkerCapacityBytes,StoreHooks);
        },
        [Weak,ExpectedEpoch,Callback](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe>&& Entry) mutable
        {
            const auto Pinned=Weak.Pin();
            if (!Pinned.IsValid() || Pinned->Epoch!=ExpectedEpoch || Pinned->IsQuarantined()) Entry.Reset();
            (*Callback)(MoveTemp(Entry));
        },8*ReadCapacity+DiskCachePrivate::WorkspaceBytes);
    return bAccepted;
}
void VaRest::Http::FDiskCache::Remove(FString Key) { RequestMaintenance(true); }
void VaRest::Http::FDiskCache::Clear() { RequestMaintenance(true); }
void VaRest::Http::FDiskCache::Put(FCacheEntry Entry,FPersistenceGuard Guard)
{
    check(IsInGameThread());
    if (IsQuarantined()) TryMaintenance();
    if (DiskCachePrivate::Controls().bExhausted || (bMaintenancePending && !bMaintenanceInFlight)
        || !DiskCachePrivate::EntryBounds(Entry)) return;
    const int64 EncodedUpper=((Entry.Body.Num()+2LL)/3)*4+DiskCachePrivate::MetadataEncodedBytes;
    if (EncodedUpper>DiskCachePrivate::MaxRecordBytes) return;
    const uint64 RequiredControl=ControlSerial;
    const int64 Reservation=PersistenceGuardReservation(EVaRestBodyKind::Binary,Entry.ContentType,Entry.Body.Num())
        +8*EncodedUpper+DiskCachePrivate::WorkspaceBytes+Entry.Body.GetAllocatedSize()+Entry.Charge();
    const auto StoreHooks=DiskCachePrivate::CaptureHooks(Directory);
#if WITH_DEV_AUTOMATION_TESTS
    const auto Hooks=Testing::PersistenceHooksFor(Directory);
#endif
    LaunchPersistence([Store=AsShared(),Value=MoveTemp(Entry),Guard=MoveTemp(Guard),RequiredControl,StoreHooks
#if WITH_DEV_AUTOMATION_TESTS
        ,Hooks
#endif
    ]() mutable
    {
#if WITH_DEV_AUTOMATION_TESTS
        if (Hooks.IsValid()) Testing::WaitPersistenceBarrier(Hooks->BeforeCacheWrite);
#endif
        FIoResult Result;
        if (Store->WorkerAppliedControlSerial!=RequiredControl) return Result;
        const int64 Capacity=Store->WorkerCapacityBytes;
        if (ReadCacheMetadata(Value.Headers,0,0,Value.Metadata.StoredUtc).bNoStore
            || !Guard.AllowsBody(EVaRestBodyKind::Binary,Value.ContentType,Value.Body)) return Result;
        Value.Headers=Guard.SafeHeaders(Value.Headers);
        // A replacement always has a fresh immutable path, including repeated Put of one DTO.
        Value.Generation=FGuid::NewGuid();
        const TArray<uint8> Encoded=EncodeCacheRecord(Value);
        if (Encoded.IsEmpty() || Encoded.Num()>Capacity || Encoded.Num()>DiskCachePrivate::MaxRecordBytes || Guard.ContainsSecret(Encoded)) return Result;
        auto Scanned=DiskCachePrivate::Scan(Store->Directory,StoreHooks);
        if (!Scanned.Status.bOk || !DiskCachePrivate::Trim(Store->Directory,Scanned.Records,Capacity,Encoded.Num(),StoreHooks)) return Result;
        const FString Path=FPaths::Combine(Store->Directory,Value.Key+TEXT(".")+Value.Generation.ToString(EGuidFormats::DigitsLower)+TEXT(".json"));
        // Retained bytes + incoming are <= Capacity. Atomic write owns one additional
        // complete temporary record at most; WorkingBytes <= Capacity + MaxRecordBytes.
#if WITH_DEV_AUTOMATION_TESTS
        if (StoreHooks.IsValid() && StoreHooks->WriteFault.IsSet()) Result=Testing::WriteWithFault(Path,Encoded,*StoreHooks->WriteFault);
        else
#endif
            Result=WriteNewFile(Path,Encoded);
        if (Result.bOk)
            for (const auto& Record : Scanned.Records)
                if (FStringView(*Record.Name,64)==Value.Key) DiskCachePrivate::Delete(FPaths::Combine(Store->Directory,Record.Name),StoreHooks);
        return Result;
    },[](FIoResult&&){},Reservation);
}

#if WITH_DEV_AUTOMATION_TESTS
VaRest::Http::Testing::FScopedCacheControls::FScopedCacheControls()
{
    check(IsInGameThread()); check(PersistenceIdle()); check(!DiskCachePrivate::TestControls);
    auto* Created=new DiskCachePrivate::FControls(); Owned=Created; DiskCachePrivate::TestControls=Created;
}
VaRest::Http::Testing::FScopedCacheControls::~FScopedCacheControls()
{
    check(IsInGameThread()); check(PersistenceIdle()); check(DiskCachePrivate::TestControls==Owned);
    DiskCachePrivate::TestControls=nullptr; delete static_cast<DiskCachePrivate::FControls*>(Owned);
}
VaRest::Http::Testing::FScopedCacheStoreHooks::FScopedCacheStoreHooks(FString InRoot)
    : Root(MoveTemp(InRoot)),Hooks(MakeShared<FCacheStoreHooks,ESPMode::ThreadSafe>())
{
    check(IsInGameThread());
    FPaths::NormalizeDirectoryName(Root); Root=FPaths::ConvertRelativePathToFull(Root); FPaths::CollapseRelativeDirectories(Root);
    DiskCachePrivate::HookRegistry().Add(Root,Hooks);
}
VaRest::Http::Testing::FScopedCacheStoreHooks::~FScopedCacheStoreHooks()
{
    check(IsInGameThread());
    if (Hooks->AfterRead.IsValid()) Hooks->AfterRead->Released=true;
    DiskCachePrivate::HookRegistry().Remove(Root);
}
int32 VaRest::Http::Testing::DirtyCacheControlsForTest() { return DiskCachePrivate::Controls().Dirty.Num(); }
int32 VaRest::Http::Testing::CacheRegistryForTest() { return DiskCachePrivate::Controls().Registry.Num(); }
#endif
