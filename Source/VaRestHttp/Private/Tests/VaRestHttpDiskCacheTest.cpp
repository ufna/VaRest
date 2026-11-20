// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS
#include "VaRestDiskCache.h"
#include "VaRestHttpTesting.h"
#include "VaRestIdentity.h"
#include "VaRestJson.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/LowLevelMemTracker.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformFileManager.h"
#if PLATFORM_LINUX
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <cerrno>
#endif


namespace
{
    // Opt-in observations of existing test-owned work; this never changes executor limits or admission.
    struct FVaRestM7DiskMeasure
    {
        struct FMark { double At=0;uint64 Launched=0,Delivered=0;int64 Pending=0; };
        bool Enabled=false;FString Directory,Path;FVaRestJson Data,Rows;
        explicit FVaRestM7DiskMeasure(const TCHAR* Selector,const TCHAR* Leaf)
        {
            Enabled=FParse::Value(FCommandLine::Get(),TEXT("VaRestM7DiskReportDir="),Directory);
            if (!Enabled) return;
            Path=FPaths::Combine(Directory,Leaf);Data=FVaRestJson::Object();Rows=FVaRestJson::Array();
            Data.Set(TEXT("schema"),TEXT("VaRest.M7.DiskAdmissionMeasurement.1"));Data.Set(TEXT("selector"),Selector);
            Data.Set(TEXT("default_disk_policy_bytes"),64LL*1024*1024);Data.Set(TEXT("default_read_logical_reservation_bytes"),112LL*1024*1024);
            Data.Set(TEXT("executor_logical_limit_bytes"),128LL*1024*1024);Data.Set(TEXT("runtime_LLM_enabled"),bool(LLM_IS_ENABLED()));
            Data.Set(TEXT("notes"),TEXT("Existing test workloads; no timing threshold. GT submission and time through owned completion/drain are separate; asynchronous time includes IO, queue and GT pumping, not pure disk service. Fixture preparation is outside measured submissions. Existing Find observation-helper setup is included where that helper is the named submitted operation. Logical charges are not RSS. A successful physical12MiB read is unmeasured. Scripted HTTP counts, where present, use fake transport; no physical network claim. Report write is outside measured cache work."));
        }
        bool Valid() const { return !Enabled || (!FPaths::IsRelative(Directory) && IFileManager::Get().DirectoryExists(*Directory) && !IFileManager::Get().FileExists(*Path)); }
        FMark Mark(bool bBefore=false) const
        {
            if (!Enabled) return {};
            FMark Result;if (!bBefore) Result.At=FPlatformTime::Seconds();
            Result.Launched=VaRest::Http::PersistenceCounters().Launched.load();Result.Delivered=VaRest::Http::PersistenceCounters().Delivered.load();Result.Pending=VaRest::Http::Testing::PersistencePendingBytesForTest();
            if (bBefore) Result.At=FPlatformTime::Seconds();return Result;
        }
        void Record(const TCHAR* Name,const FMark& Before,const FMark& Submitted,const FMark& Completed,bool bCompletion=true)
        {
            if (!Enabled) return;
            FVaRestJson Row=FVaRestJson::Object();Row.Set(TEXT("stage"),Name);Row.Set(TEXT("GT_submission_ms"),(Submitted.At-Before.At)*1000.0);
            Row.Set(TEXT("executor_launches_during_submission"),Submitted.Launched-Before.Launched);Row.Set(TEXT("pending_logical_bytes_after_submission"),Submitted.Pending);
            Row.Set(TEXT("owned_completion_observed"),bCompletion);
            if (bCompletion)
            {
                Row.Set(TEXT("submission_to_owned_completion_or_drain_ms"),(Completed.At-Before.At)*1000.0);
                Row.Set(TEXT("after_submission_to_owned_completion_or_drain_ms"),(Completed.At-Submitted.At)*1000.0);
                Row.Set(TEXT("executor_launches_through_completion"),Completed.Launched-Before.Launched);Row.Set(TEXT("executor_deliveries_through_completion"),Completed.Delivered-Before.Delivered);
                Row.Set(TEXT("pending_logical_bytes_after_completion"),Completed.Pending);
            }
            Rows.Add(Row);
        }
        bool Finish()
        {
            if (!Enabled) return true;
            Data.Set(TEXT("stages"),Rows);
            return FFileHelper::SaveStringToFile(Data.ToString(),*Path,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,&IFileManager::Get(),FILEWRITE_NoReplaceExisting);
        }
    };
}

namespace VaRest::Http::CacheStoreTestPrivate
{
    FCacheEntry Entry(const TCHAR* Name)
    {
        FCacheEntry Value;
        Value.Key=HashFields(TEXT("cache-test"),TArray<FString>{Name});
        Value.Body={97,98,99}; Value.ContentType=TEXT("text/plain");
        Value.Headers.Add(TEXT("Cache-Control"),TEXT("max-age=60"));
        Value.Metadata=ReadCacheMetadata(Value.Headers,0,0,FDateTime(2026,10,23));
        return Value;
    }
    struct FFound
    {
        int32 Calls=0;
        bool bGameThread=true,bAccepted=false;
        TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Value;
    };
    TSharedRef<FFound,ESPMode::ThreadSafe> Find(const TSharedRef<FDiskCache,ESPMode::ThreadSafe>& Store,const FString& Key,
        FDateTime Utc=FDateTime(2026,10,23))
    {
        const auto Seen=MakeShared<FFound,ESPMode::ThreadSafe>();
        Seen->bAccepted=Store->Find(Key,20,Utc,[Seen](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Value)
        { ++Seen->Calls; Seen->bGameThread&=IsInGameThread(); Seen->Value=MoveTemp(Value); });
        return Seen;
    }
    TSharedRef<FIoResult,ESPMode::ThreadSafe> List(const FString& Root)
    {
        const auto Seen=MakeShared<FIoResult,ESPMode::ThreadSafe>();
        LaunchPersistence([Root] { return ListFiles(Root); },[Seen](FIoResult&& Value) { *Seen=MoveTemp(Value); });
        return Seen;
    }
    bool WaitEntered(const TSharedRef<Testing::FPersistenceBarrier,ESPMode::ThreadSafe>& Gate)
    {
        const double Deadline=FPlatformTime::Seconds()+5;
        while (!Gate->Entered.load() && FPlatformTime::Seconds()<Deadline) FPlatformProcess::SleepNoStats(0.001f);
        return Gate->Entered.load() && !Gate->TimedOut.load();
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheCorruptMetadataTest,"VaRest.Http.CacheStore.CorruptMetadata",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheCorruptMetadataTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    FScopedPersistenceTest Disk;
    const auto Results=MakeShared<TArray<TPair<FString,bool>>,ESPMode::ThreadSafe>();
    LaunchPersistence([Results]
    {
        const auto Check=[Results](const TCHAR* Name,bool bPassed){ Results->Emplace(Name,bPassed); };
        auto Value=CacheStoreTestPrivate::Entry(TEXT("codec"));
        const FDateTime Utc(2026,10,23);
        const auto Bytes=EncodeCacheRecord(Value);
        auto Good=DecodeCacheRecord(Bytes,Value.Key,20,Utc,1024);
        Check(TEXT("valid baseline/body"),Good.IsSet() && Good->Body==Value.Body);
        const auto Parsed=FVaRestJson::Parse(Bytes);
        Check(TEXT("record parses"),Parsed.HasValue());
        if (Parsed.HasValue())
        {
            for (const TCHAR* Corruption : {TEXT("version"),TEXT("bodySha256"),TEXT("key"),TEXT("generation"),TEXT("noCache"),
                TEXT("mustRevalidate"),TEXT("lifetime"),TEXT("initialAge"),TEXT("headers"),TEXT("contentType"),TEXT("storedUtc"),TEXT("usedUtc"),TEXT("body")})
            {
                auto Bad=Parsed.GetValue().Clone();
                const FString Name(Corruption);
                if (Name==TEXT("version")) Bad.Set(Corruption,99);
                else if (Name==TEXT("bodySha256")) Bad.Set(Corruption,FString::ChrN(64,TEXT('0')));
                else if (Name==TEXT("key")) Bad.Set(Corruption,TEXT("../escape"));
                else if (Name==TEXT("generation")) Bad.Set(Corruption,TEXT("00000000000000000000000000000000"));
                else if (Name==TEXT("noCache") || Name==TEXT("mustRevalidate")) Bad.Set(Corruption,TEXT("false"));
                else if (Name==TEXT("lifetime") || Name==TEXT("initialAge")) Bad.Set(Corruption,-1);
                else if (Name==TEXT("headers")) Bad.Set(Corruption,FVaRestJson::Array(FVaRestJson::Array(TEXT("Set-Cookie"),TEXT("private"))));
                else if (Name==TEXT("contentType")) Bad.Set(Corruption,FString::ChrN(1025,TEXT('x')));
                else if (Name==TEXT("body")) Bad.Set(Corruption,TEXT("Y===junk"));
                else Bad.Set(Corruption,TEXT("bad-time"));
                TArray<uint8> Encoded;
                Check(Corruption,Bad.TryToUtf8(Encoded).HasValue() && !DecodeCacheRecord(Encoded,Value.Key,20,Utc,1024).IsSet());
            }
        }
        TArray<uint8> Truncated=Bytes; Truncated.SetNum(Truncated.Num()/2);
        Check(TEXT("truncated"),!DecodeCacheRecord(Truncated,Value.Key,20,Utc,1024).IsSet());
        Check(TEXT("body cap"),!DecodeCacheRecord(Bytes,Value.Key,20,Utc,2).IsSet());
        Check(TEXT("expected key"),!DecodeCacheRecord(Bytes,CacheStoreTestPrivate::Entry(TEXT("other")).Key,20,Utc,1024).IsSet());
        Value.Metadata.StoredUtc=Utc+FTimespan::FromDays(1);
        const auto Future=DecodeCacheRecord(EncodeCacheRecord(Value),Value.Key,20,Utc,1024);
        Check(TEXT("future record stale"),Future.IsSet() && !Future->Metadata.IsFresh(20));
        Value=CacheStoreTestPrivate::Entry(TEXT("headers"));
        Value.Headers=FVaRestHeaders();
        for (int32 Index=0;Index<128;++Index) Value.Headers.Add(FString::Printf(TEXT("X-Header-%d"),Index),FString::ChrN(400,TEXT('a')));
        Check(TEXT("128 valid header schema fits bounded parser"),DecodeCacheRecord(EncodeCacheRecord(Value),Value.Key,20,Utc,1024).IsSet());
        Value.Headers.Add(TEXT("X-Extra"),TEXT("x"));
        Check(TEXT("129 headers refused"),!DecodeCacheRecord(EncodeCacheRecord(Value),Value.Key,20,Utc,1024).IsSet());
        Value=CacheStoreTestPrivate::Entry(TEXT("no-store")); Value.Headers.Set(TEXT("Cache-Control"),TEXT("no-store"));
        Check(TEXT("no-store refused"),!DecodeCacheRecord(EncodeCacheRecord(Value),Value.Key,20,Utc,1024).IsSet());
        auto Huge=FVaRestJson::Object(); FVaRestJson Nodes=FVaRestJson::Array();
        for (int32 Index=0;Index<3000;++Index) Nodes.Add(0);
        Huge.Set(TEXT("flood"),Nodes); TArray<uint8> Flood; Huge.TryToUtf8(Flood);
        Check(TEXT("lexical flood refused"),!DecodeCacheRecord(Flood,Value.Key,20,Utc,1024*1024).IsSet());
        FIoResult Result; Result.bOk=true; return Result;
    },[](FIoResult&&){},16*1024*1024);
    if (!TestTrue(TEXT("codec worker drained"),Disk.Drain())) return false;
    for (const auto& Result : *Results) TestTrue(Result.Key,Result.Value);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheSharedCapacityTest,"VaRest.Http.CacheStore.SharedCapacityLifetime",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheSharedCapacityTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheStoreTestPrivate;
    FScopedPersistenceTest Disk;
    const FString Root=FPaths::Combine(Disk.Root,TEXT("Store"));
    const auto Value=Entry(TEXT("capacity"));
    TSharedPtr<FDiskCache,ESPMode::ThreadSafe> Store=FDiskCache::Open(Root,1024*1024);
    Store->Put(Value,FPersistenceGuard());
    if (!TestTrue(TEXT("initial write"),Disk.Drain())) return false;
    TSharedPtr<FDiskCache,ESPMode::ThreadSafe> Smaller=FDiskCache::Open(Root,1);
    if (!TestTrue(TEXT("serialized capacity reduction"),Disk.Drain())) return false;
    Smaller.Reset();
    TSharedPtr<FDiskCache,ESPMode::ThreadSafe> Larger=FDiskCache::Open(Root,1024*1024);
    TestTrue(TEXT("same live store"),Larger==Store);
    Store->Put(Value,FPersistenceGuard());
    auto Seen=Find(Store.ToSharedRef(),Value.Key);
    if (!TestTrue(TEXT("smaller cap still enforced"),Disk.Drain())) return false;
    TestFalse(TEXT("releasing smaller registrant does not recover capacity"),Seen->Value.IsValid());
    Store.Reset(); Larger.Reset();
    if (!TestTrue(TEXT("all old workers released"),Disk.Drain())) return false;
    Store=FDiskCache::Open(Root,1024*1024);
    Store->Put(Value,FPersistenceGuard());
    Seen=Find(Store.ToSharedRef(),Value.Key);
    if (!TestTrue(TEXT("fresh lifetime complete"),Disk.Drain())) return false;
    TestTrue(TEXT("full reopen permits larger cap"),Seen->Value.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheDiskRecoveryTest,"VaRest.Http.CacheStore.DiskRecovery",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheDiskRecoveryTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheStoreTestPrivate;
    FVaRestM7DiskMeasure Measure(TEXT("VaRest.Http.CacheStore.DiskRecovery"),TEXT("CacheStore-DiskRecovery.json"));
    if (Measure.Enabled && !TestTrue(TEXT("fresh absolute M7 disk report destination"),Measure.Valid())) return false;
    FScopedPersistenceTest Disk;
    const uint64 Before=PersistenceCounters().OperationsOnGameThread.load();
    const FString Root=FPaths::Combine(Disk.Root,TEXT("Store"));
    auto A=Entry(TEXT("A")),B=Entry(TEXT("B")),C=Entry(TEXT("C"));
    const auto Sizes=MakeShared<int64,ESPMode::ThreadSafe>(0);
    LaunchPersistence([A,Sizes] { *Sizes=EncodeCacheRecord(A).Num(); FIoResult Result; Result.bOk=true; return Result; },[](FIoResult&&){});
    if (!TestTrue(TEXT("measurement drain"),Disk.Drain())) return false;
    TSharedPtr<FDiskCache,ESPMode::ThreadSafe> Store=FDiskCache::Open(Root,*Sizes*2);
    Store->Put(A,FPersistenceGuard()); Store->Put(B,FPersistenceGuard());
    if (!TestTrue(TEXT("two records persisted"),Disk.Drain())) return false;
    auto Found=Find(Store.ToSharedRef(),A.Key,FDateTime(2028,1,1));
    if (!TestTrue(TEXT("touch drain"),Disk.Drain())) return false;
    if (!TestTrue(TEXT("A touch succeeds"),Found->Value.IsValid())) return false;
    Store->Put(C,FPersistenceGuard());
    if (!TestTrue(TEXT("eviction drain"),Disk.Drain())) return false;
    Store.Reset();
    Store=FDiskCache::Open(Root,*Sizes*2);
    Found=Find(Store.ToSharedRef(),A.Key);
    auto Evicted=Find(Store.ToSharedRef(),B.Key);
    if (!TestTrue(TEXT("restart drain"),Disk.Drain())) return false;
    TestTrue(TEXT("recently touched A survives reopen"),Found->Value.IsValid());
    TestFalse(TEXT("oldest B evicted"),Evicted->Value.IsValid());
    auto Names=List(Root); if (!TestTrue(TEXT("listing"),Disk.Drain())) return false;
    TestTrue(TEXT("retained encoded bytes cap"),Names->bOk && Names->Names.Num()==2);
    Store->Clear(); if (!TestTrue(TEXT("clear"),Disk.Drain())) return false;
    // Default-policy usability is separate from the deliberately tiny LRU budget.
    const auto DefaultPutBefore=Measure.Mark(true);
    Store.Reset(); Store=FDiskCache::Open(Root,64LL*1024*1024);
    Store->Put(A,FPersistenceGuard()); const auto DefaultPutSubmitted=Measure.Mark();
    if (!TestTrue(TEXT("default write"),Disk.Drain())) return false;
    Measure.Record(TEXT("default_reopen_and_small_put"),DefaultPutBefore,DefaultPutSubmitted,Measure.Mark());
    const auto DefaultReadBefore=Measure.Mark(true);
    Found=Find(Store.ToSharedRef(),A.Key); const auto DefaultReadSubmitted=Measure.Mark();
    if (!TestTrue(TEXT("default read"),Disk.Drain())) return false;
    Measure.Record(TEXT("default_small_find"),DefaultReadBefore,DefaultReadSubmitted,Measure.Mark());
    if (Measure.Enabled) Measure.Data.Set(TEXT("default_small_find_hit"),Found->Value.IsValid());
    TestTrue(TEXT("default64MiB small roundtrip"),Found->Value.IsValid());
    const auto ReopenBefore=Measure.Mark(true);
    Store.Reset(); Store=FDiskCache::Open(Root,64LL*1024*1024);
    Found=Find(Store.ToSharedRef(),A.Key); const auto ReopenSubmitted=Measure.Mark();
    if (!TestTrue(TEXT("default restart"),Disk.Drain())) return false;
    Measure.Record(TEXT("default_reopen_and_find"),ReopenBefore,ReopenSubmitted,Measure.Mark());
    if (Measure.Enabled) { Measure.Data.Set(TEXT("default_reopen_hit"),Found->Value.IsValid());Measure.Data.Set(TEXT("small_body_bytes"),A.Body.Num());Measure.Data.Set(TEXT("last_callback_count"),Found->Calls); }
    TestTrue(TEXT("default64MiB recovery"),Found->Value.IsValid());
    TestTrue(TEXT("GT-only handback"),Found->bGameThread && Found->Calls==1);
    TestEqual(TEXT("all store disk IO off GT"),PersistenceCounters().OperationsOnGameThread.load(),Before);
    if (Measure.Enabled && !TestTrue(TEXT("write fresh M7 disk report"),Measure.Finish())) return false;
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheClearReadTest,"VaRest.Http.CacheStore.ClearWhileRead",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheClearReadTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheStoreTestPrivate;
    FScopedPersistenceTest Disk;
    const FString Root=FPaths::Combine(Disk.Root,TEXT("Store"));
    const auto Value=Entry(TEXT("clear-read"));
    auto Store=FDiskCache::Open(Root,1024*1024);
    Store->Put(Value,FPersistenceGuard()); if (!TestTrue(TEXT("seed write"),Disk.Drain())) return false;
    FScopedCacheStoreHooks Hooks(Root);
    auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->AfterRead=Gate;
    ON_SCOPE_EXIT { Gate->Released=true; Disk.Drain(); };
    const auto Found=Find(Store,Value.Key);
    if (!TestTrue(TEXT("worker has captured valid cached bytes"),WaitEntered(Gate))) return false;
    const uint64 Epoch=Store->GetEpoch();
    Store->Clear();
    TestTrue(TEXT("clear immediately advances epoch"),Store->GetEpoch()>Epoch);
    Gate->Released=true;
    if (!TestTrue(TEXT("clear/read drains"),Disk.Drain())) return false;
    TestEqual(TEXT("one handback"),Found->Calls,1);
    TestFalse(TEXT("late read after Clear is a miss"),Found->Value.IsValid());
    TestFalse(TEXT("maintenance completed"),Store->IsQuarantined());
    auto Again=Find(Store,Value.Key);
    if (!TestTrue(TEXT("post clear drain"),Disk.Drain())) return false;
    TestFalse(TEXT("clear removed durable bytes"),Again->Value.IsValid());
    TestFalse(TEXT("barrier did not time out"),Gate->TimedOut.load());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheFileFaultTest,"VaRest.Http.CacheStore.FileFaults",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheFileFaultTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheStoreTestPrivate;
    FScopedPersistenceTest Disk;
    const FString Root=FPaths::Combine(Disk.Root,TEXT("Store"));
    auto Store=FDiskCache::Open(Root,1024*1024);
    const auto Value=Entry(TEXT("fault"));
    if (!TestTrue(TEXT("open drain"),Disk.Drain())) return false;
    for (EFileFault Fault : {EFileFault::Write,EFileFault::Flush,EFileFault::Rename})
    {
        { FScopedCacheStoreHooks Hooks(Root); Hooks.Hooks->WriteFault=Fault;
          Store->Put(Value,FPersistenceGuard()); if (!TestTrue(TEXT("failed write drain"),Disk.Drain())) return false; }
        auto Found=Find(Store,Value.Key); auto Names=List(Root);
        if (!TestTrue(TEXT("failed write observation"),Disk.Drain())) return false;
        TestFalse(TEXT("failed best effort put is a miss"),Found->Value.IsValid());
        TestTrue(TEXT("failed write cleans every temporary"),Names->bOk && Names->Names.IsEmpty());
    }
    for (int32 Kind=0;Kind<3;++Kind)
    {
        Store->Put(Value,FPersistenceGuard()); if (!TestTrue(TEXT("read seed"),Disk.Drain())) return false;
        TSharedPtr<FFound,ESPMode::ThreadSafe> Found;
        { FScopedCacheStoreHooks Hooks(Root);
          Hooks.Hooks->bFailRead=Kind==0; Hooks.Hooks->bDeleteBeforeRead=Kind==2;
          if (Kind==1) Hooks.Hooks->ReadFault=EFileFault::ShortRead;
          Found=Find(Store,Value.Key); if (!TestTrue(TEXT("read failure drains"),Disk.Drain())) return false; }
        TestFalse(TEXT("read/short read/deleted-between-index-and-read is miss"),Found->Value.IsValid());
        auto Names=List(Root); if (!TestTrue(TEXT("failed read cleanup"),Disk.Drain())) return false;
        TestTrue(TEXT("bad record removed"),Names->bOk && Names->Names.IsEmpty());
    }
    Store->Put(Value,FPersistenceGuard()); if (!TestTrue(TEXT("healthy write"),Disk.Drain())) return false;
    auto Found=Find(Store,Value.Key); if (!TestTrue(TEXT("healthy read"),Disk.Drain())) return false;
    TestTrue(TEXT("faults do not poison later best effort stores"),Found->Value.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheControlsTest,"VaRest.Http.CacheStore.MaintenanceQuarantine",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheControlsTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheStoreTestPrivate;
    FScopedPersistenceTest Disk;
    FScopedCacheControls Controls;
    const FString Root=FPaths::Combine(Disk.Root,TEXT("Store"));
    const auto Value=Entry(TEXT("sticky"));
    TSharedPtr<FDiskCache,ESPMode::ThreadSafe> Store=FDiskCache::Open(Root,1024*1024);
    Store->Put(Value,FPersistenceGuard()); if (!TestTrue(TEXT("seed"),Disk.Drain())) return false;
    const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
    ON_SCOPE_EXIT { Gate->Released=true; Disk.Drain(); };
    TestTrue(TEXT("occupy first slot"),LaunchPersistence([Gate] { WaitPersistenceBarrier(Gate); return FIoResult(); },[](FIoResult&&){}));
    if (!TestTrue(TEXT("first slot held"),WaitEntered(Gate))) return false;
    int32 Accepted=1;
    for (int32 Index=1;Index<8192;++Index)
        if (LaunchPersistence([] { return FIoResult(); },[](FIoResult&&){})) ++Accepted;
    if (!TestEqual(TEXT("actual full callback map"),Accepted,8192)) return false;
    Store->Clear();
    TestTrue(TEXT("refused control quarantines"),Store->IsQuarantined());
    Store->Put(Value,FPersistenceGuard()); auto Refused=Find(Store.ToSharedRef(),Value.Key);
    TestFalse(TEXT("full admission blocks reuse"),Refused->Value.IsValid());
    Store.Reset(); TestEqual(TEXT("intent retained without clients"),DirtyCacheControlsForTest(),1);
    Gate->Released=true; if (!TestTrue(TEXT("release full slots"),Disk.Drain())) return false;
    Store=FDiskCache::Open(Root,1024*1024);
    auto Found=Find(Store.ToSharedRef(),Value.Key);
    if (!TestTrue(TEXT("reopen retries refused clear"),Disk.Drain())) return false;
    TestFalse(TEXT("old durable bytes cannot resurrect"),Found->Value.IsValid());
    TestEqual(TEXT("successful control releases intent"),DirtyCacheControlsForTest(),0);
    // Both listing and actual deletion failures retain intent across holder release.
    for (int32 Kind=0;Kind<3;++Kind)
    {
        Store->Put(Value,FPersistenceGuard()); if (!TestTrue(TEXT("failure seed"),Disk.Drain())) return false;
        { FScopedCacheStoreHooks Hooks(Root); Hooks.Hooks->bFailList=Kind==0; Hooks.Hooks->bFailDelete=Kind!=0;
          if (Kind==2) Store=FDiskCache::Open(Root,1); else Store->Remove(Value.Key);
          if (!TestTrue(TEXT("failed maintenance drains without retry loop"),Disk.Drain())) return false;
          TestTrue(TEXT("failed maintenance quarantines"),Store->IsQuarantined());
          Store.Reset(); TestEqual(TEXT("failed intent retained"),DirtyCacheControlsForTest(),1);
          Store=FDiskCache::Open(Root,1024*1024);
          Store->Put(Value,FPersistenceGuard()); Found=Find(Store.ToSharedRef(),Value.Key);
          if (!TestTrue(TEXT("failed retry and dependent jobs drain"),Disk.Drain())) return false;
          TestFalse(TEXT("failed worker control forbids read and put"),Found->Value.IsValid()); }
        Found=Find(Store.ToSharedRef(),Value.Key);
        if (!TestTrue(TEXT("next access retries maintenance"),Disk.Drain())) return false;
        TestFalse(TEXT("successful retry leaves old bytes gone"),Found->Value.IsValid());
        TestFalse(TEXT("only success clears quarantine"),Store->IsQuarantined());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheCapacityFailureTest,"VaRest.Http.CacheStore.CapacityFailure",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheCapacityFailureTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheStoreTestPrivate;
    FScopedPersistenceTest Disk;
    const FString Root=FPaths::Combine(Disk.Root,TEXT("Store"));
    auto A=Entry(TEXT("A")),B=Entry(TEXT("B"));
    const auto Size=MakeShared<int64,ESPMode::ThreadSafe>(0);
    LaunchPersistence([A,Size]{ *Size=EncodeCacheRecord(A).Num(); return FIoResult(); },[](FIoResult&&){});
    if (!TestTrue(TEXT("measure"),Disk.Drain())) return false;
    auto Store=FDiskCache::Open(Root,*Size);
    Store->Put(A,FPersistenceGuard()); if (!TestTrue(TEXT("seed"),Disk.Drain())) return false;
    for (int32 Kind=0;Kind<2;++Kind)
    {
        { FScopedCacheStoreHooks Hooks(Root); Hooks.Hooks->bFailDelete=Kind==0; Hooks.Hooks->bFailList=Kind==1;
          Store->Put(B,FPersistenceGuard()); if (!TestTrue(TEXT("failed capacity admission"),Disk.Drain())) return false; }
        auto Found=Find(Store,B.Key); auto Names=List(Root);
        if (!TestTrue(TEXT("failure observation"),Disk.Drain())) return false;
        TestFalse(TEXT("failed trim/list never admits overcapacity write"),Found->Value.IsValid());
        TestTrue(TEXT("one original encoded record remains"),Names->bOk && Names->Names.Num()==1);
    }
    Store->Put(B,FPersistenceGuard()); if (!TestTrue(TEXT("healthy eviction"),Disk.Drain())) return false;
    auto Found=Find(Store,B.Key); if (!TestTrue(TEXT("healthy observation"),Disk.Drain())) return false;
    TestTrue(TEXT("replacement succeeds after failure"),Found->Value.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheCorruptFilesTest,"VaRest.Http.CacheStore.CorruptFiles",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheCorruptFilesTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheStoreTestPrivate;
    FScopedPersistenceTest Disk;
    const FString Root=FPaths::Combine(Disk.Root,TEXT("Store"));
    auto Store=FDiskCache::Open(Root,64LL*1024*1024);
    const auto Value=Entry(TEXT("corrupt"));
    if (!TestTrue(TEXT("open"),Disk.Drain())) return false;
    for (int32 Kind=0;Kind<6;++Kind)
    {
        const auto Written=MakeShared<bool,ESPMode::ThreadSafe>(false);
        LaunchPersistence([Root,Value,Kind]
        {
            auto Record=EncodeCacheRecord(Value);
            if (Kind==1) Record.SetNum(Record.Num()/2);
            else if (Kind==4) Record.Init('x',13*1024*1024);
            else
            {
                auto Parsed=FVaRestJson::Parse(Record);
                if (Parsed.HasError()) return FIoResult();
                auto Json=Parsed.StealValue();
                if (Kind==0) Json.Set(TEXT("bodySha256"),FString::ChrN(64,TEXT('0')));
                else if (Kind==2) Json.Set(TEXT("version"),2);
                else if (Kind==3) Json.Set(TEXT("key"),TEXT("../outside"));
                else Json.Set(TEXT("generation"),FGuid::NewGuid().ToString(EGuidFormats::DigitsLower));
                if (Json.TryToUtf8(Record).HasError()) return FIoResult();
            }
            return WriteNewFile(FPaths::Combine(Root,Value.Key+TEXT(".")+Value.Generation.ToString(EGuidFormats::DigitsLower)+TEXT(".json")),Record);
        },[Written](FIoResult&& Result){ *Written=Result.bOk; },64*1024*1024);
        if (!TestTrue(TEXT("corrupt seed worker"),Disk.Drain())) return false;
        if (!TestTrue(TEXT("corrupt fixture actually written"),*Written)) return false;
        auto Found=Find(Store,Value.Key); if (!TestTrue(TEXT("corrupt read worker"),Disk.Drain())) return false;
        TestFalse(TEXT("corrupt disk is miss, not application failure"),Found->Value.IsValid());
        auto Names=List(Root); if (!TestTrue(TEXT("corrupt cleanup worker"),Disk.Drain())) return false;
        TestTrue(TEXT("corrupt record deleted"),Names->bOk && Names->Names.IsEmpty());
    }
#if PLATFORM_LINUX
    const FString Outside=FPaths::ConvertRelativePathToFull(FPaths::Combine(Disk.Root,TEXT("outside")));
    const FString Link=FPaths::ConvertRelativePathToFull(FPaths::Combine(Root,Value.Key+TEXT(".")+Value.Generation.ToString(EGuidFormats::DigitsLower)+TEXT(".json")));
    const auto Linked=MakeShared<bool,ESPMode::ThreadSafe>(false);
    LaunchPersistence([Outside,Link,Root,Linked]
    {
        auto Result=WriteNewFile(Outside,TArray<uint8>{1,2,3,4});
        if (!Result.bOk) return Result;
        FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Root);
        *Linked=symlink(TCHAR_TO_UTF8(*FPaths::ConvertRelativePathToFull(Outside)),TCHAR_TO_UTF8(*Link))==0;
        return Result;
    },[](FIoResult&&){});
    if (!TestTrue(TEXT("symlink fixture"),Disk.Drain())) return false;
    if (!TestTrue(TEXT("actual outside symlink"),*Linked)) return false;
    auto Found=Find(Store,Value.Key); Store->Clear();
    if (!TestTrue(TEXT("symlink read clear"),Disk.Drain())) return false;
    TestFalse(TEXT("symlink ignored"),Found->Value.IsValid());
    const auto Preserved=MakeShared<bool,ESPMode::ThreadSafe>(false);
    LaunchPersistence([Outside,Link,Preserved]
    {
        auto Result=ReadBoundedFile(Outside,4);
        struct stat Info;
        *Preserved=Result.bOk && Result.Bytes==TArray<uint8>({1,2,3,4})
            && lstat(TCHAR_TO_UTF8(*Link),&Info)==0 && S_ISLNK(Info.st_mode);
        FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*Link);
        return Result;
    },[](FIoResult&&){});
    if (!TestTrue(TEXT("symlink observation"),Disk.Drain())) return false;
    TestTrue(TEXT("neither link nor target traversed/removed"),*Preserved);
    const FString LinkedRoot=FPaths::ConvertRelativePathToFull(FPaths::Combine(Disk.Root,TEXT("linked-root")));
    const auto RootWitness=MakeShared<bool,ESPMode::ThreadSafe>(false);
    LaunchPersistence([Root,LinkedRoot,Value,RootWitness]
    {
        auto Result=WriteNewFile(FPaths::Combine(Root,Value.Key+TEXT(".")+Value.Generation.ToString(EGuidFormats::DigitsLower)+TEXT(".json")),EncodeCacheRecord(Value));
        if (!Result.bOk) return Result;
        struct stat Info;
        *RootWitness=symlink(TCHAR_TO_UTF8(*FPaths::ConvertRelativePathToFull(Root)),TCHAR_TO_UTF8(*LinkedRoot))==0
            && lstat(TCHAR_TO_UTF8(*LinkedRoot),&Info)==0 && S_ISLNK(Info.st_mode);
        return Result;
    },[](FIoResult&&){});
    if (!TestTrue(TEXT("linked root fixture"),Disk.Drain()) || !TestTrue(TEXT("real linked root"),*RootWitness)) return false;
    auto ThroughLink=FDiskCache::Open(LinkedRoot,1024*1024);
    ThroughLink->Clear(); ThroughLink->Put(Value,FPersistenceGuard()); auto LinkedFound=Find(ThroughLink,Value.Key);
    if (!TestTrue(TEXT("linked root jobs drain"),Disk.Drain())) return false;
    TestTrue(TEXT("linked root remains quarantined"),ThroughLink->IsQuarantined());
    TestFalse(TEXT("linked root never serves outside bytes"),LinkedFound->Value.IsValid());
    auto Original=Find(Store,Value.Key); if (!TestTrue(TEXT("outside original read"),Disk.Drain())) return false;
    TestTrue(TEXT("clear through linked root preserves original"),Original->Value.IsValid());
    LaunchPersistence([LinkedRoot] { unlink(TCHAR_TO_UTF8(*LinkedRoot)); return FIoResult(); },[](FIoResult&&){});
    if (!TestTrue(TEXT("linked root fixture cleanup"),Disk.Drain())) return false;
    // Fulfil retained control now that the link is gone; it was never a durable clear.
    ThroughLink->Clear(); if (!TestTrue(TEXT("missing root maintenance"),Disk.Drain())) return false;
#endif
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheCountTest,"VaRest.Http.CacheStore.EmptyBodyCountCap",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheCountTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheStoreTestPrivate;
    FVaRestM7DiskMeasure Measure(TEXT("VaRest.Http.CacheStore.EmptyBodyCountCap"),TEXT("CacheStore-EmptyBodyCountCap.json"));
    if (Measure.Enabled && !TestTrue(TEXT("fresh absolute M7 disk report destination"),Measure.Valid())) return false;
    FScopedPersistenceTest Disk;
    const FString Root=FPaths::Combine(Disk.Root,TEXT("Store"));
    const auto Count=MakeShared<int32,ESPMode::ThreadSafe>(0);
    LaunchPersistence([Root,Count]
    {
        auto Value=Entry(TEXT("empty")); Value.Body.Reset();
        for (int32 Index=0;Index<4097;++Index)
        {
            Value.Key=HashFields(TEXT("empty"),TArray<FString>{FString::FromInt(Index)});
            auto Result=WriteNewFile(FPaths::Combine(Root,Value.Key+TEXT(".")+Value.Generation.ToString(EGuidFormats::DigitsLower)+TEXT(".json")),EncodeCacheRecord(Value));
            if (!Result.bOk) return Result;
            ++*Count;
        }
        return WriteNewFile(FPaths::Combine(Root,TEXT("abandoned.part")),TArray<uint8>{1});
    },[](FIoResult&&){},16*1024*1024);
    if (!TestTrue(TEXT("4097 encoded empty fixtures"),Disk.Drain(60))) return false;
    if (!TestEqual(TEXT("all fixtures exist"),*Count,4097)) return false;
    const auto TrimBefore=Measure.Mark(true);auto Store=FDiskCache::Open(Root,64LL*1024*1024);const auto TrimSubmitted=Measure.Mark();
    if (!TestTrue(TEXT("bounded recovery trim"),Disk.Drain(30))) return false;
    Measure.Record(TEXT("actual_4097_record_recovery_trim"),TrimBefore,TrimSubmitted,Measure.Mark());
    if (Measure.Enabled) { Measure.Data.Set(TEXT("fixture_records_written_before_timing"),*Count);Measure.Data.Set(TEXT("fixture_preparation_timed"),false); }
    auto Names=List(Root); if (!TestTrue(TEXT("bounded listing"),Disk.Drain())) return false;
    TestTrue(TEXT("4096 record cap and abandoned temp cleaned"),Names->bOk && Names->Names.Num()==4096);
    auto Fresh=Entry(TEXT("new-empty")); Fresh.Body.Reset();const auto ReplaceBefore=Measure.Mark(true);
    Store->Put(Fresh,FPersistenceGuard());const auto ReplaceSubmitted=Measure.Mark();
    if (!TestTrue(TEXT("count-cap replacement"),Disk.Drain(30))) return false;
    Measure.Record(TEXT("put_at_4096_record_cap"),ReplaceBefore,ReplaceSubmitted,Measure.Mark());
    const auto CountReadBefore=Measure.Mark(true);Names=List(Root); auto Found=Find(Store,Fresh.Key);const auto CountReadSubmitted=Measure.Mark();
    if (!TestTrue(TEXT("new empty record observed"),Disk.Drain(30))) return false;
    Measure.Record(TEXT("4096_record_listing_and_find"),CountReadBefore,CountReadSubmitted,Measure.Mark());
    TestTrue(TEXT("count remains bounded"),Names->bOk && Names->Names.Num()==4096);
    TestTrue(TEXT("empty body is a valid hit"),Found->Value.IsValid() && Found->Value->Body.IsEmpty());
    if (Measure.Enabled) { Measure.Data.Set(TEXT("record_count_after_replacement"),Names->Names.Num());Measure.Data.Set(TEXT("empty_body_hit"),Found->Value.IsValid() && Found->Value->Body.IsEmpty());Measure.Data.Set(TEXT("callback_count"),Found->Calls); }
    if (Measure.Enabled && !TestTrue(TEXT("write fresh M7 disk report"),Measure.Finish())) return false;
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheGuardBudgetTest,"VaRest.Http.CacheStore.GuardAndWorkspace",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheGuardBudgetTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheStoreTestPrivate;
    FVaRestM7DiskMeasure Measure(TEXT("VaRest.Http.CacheStore.GuardAndWorkspace"),TEXT("CacheStore-GuardAndWorkspace.json"));
    if (Measure.Enabled && !TestTrue(TEXT("fresh absolute M7 disk report destination"),Measure.Valid())) return false;
    FScopedPersistenceTest Disk;
    const FString Root=FPaths::Combine(Disk.Root,TEXT("Store"));
    auto Store=FDiskCache::Open(Root,64LL*1024*1024);
    if (!TestTrue(TEXT("open"),Disk.Drain())) return false;
    for (int32 Kind=0;Kind<4;++Kind)
    {
        auto Value=Entry(TEXT("secret"));
        FPersistenceGuard Snapshot,Current;
        Snapshot.AddSecret(TEXT("abc")); Current.AddSecret(TEXT("ABC")); Current.UnionWith(Snapshot);
        if (Kind==1) Value.Body={65,66,67};
        if (Kind==2) { Value.Body={1,2,3}; Value.ContentType=TEXT("application/abc"); }
        if (Kind==3)
        {
            const ANSICHAR* Text="{\"password\":\"unregistered\"}";
            Value.Body=TArray<uint8>(reinterpret_cast<const uint8*>(Text),FCStringAnsi::Strlen(Text));
            Value.ContentType=TEXT("application/json");
        }
        Store->Put(Value,MoveTemp(Current)); if (!TestTrue(TEXT("guard candidate"),Disk.Drain())) return false;
        auto Found=Find(Store,Value.Key); if (!TestTrue(TEXT("guard observation"),Disk.Drain())) return false;
        TestFalse(TEXT("full body/current+snapshot/serialized/structural guard skips unsafe store"),Found->Value.IsValid());
    }
    auto Safe=Entry(TEXT("safe")); Safe.Headers.Add(TEXT("Set-Cookie"),TEXT("private")); Safe.Headers.Add(TEXT("X-Safe"),TEXT("ok"));
    Store->Put(Safe,FPersistenceGuard()); if (!TestTrue(TEXT("safe headers put"),Disk.Drain())) return false;
    auto Found=Find(Store,Safe.Key); if (!TestTrue(TEXT("safe headers read"),Disk.Drain())) return false;
    if (TestTrue(TEXT("safe record retained"),Found->Value.IsValid()))
    {
        TestTrue(TEXT("safe header retained"),Found->Value->Headers.Find(TEXT("X-Safe"))!=nullptr);
        TestTrue(TEXT("auth/session header stripped"),Found->Value->Headers.Find(TEXT("Set-Cookie"))==nullptr);
    }
    auto Huge=Entry(TEXT("over-workspace")); Huge.Body.Init('a',8*1024*1024); Huge.ContentType=TEXT("application/json");
    const uint64 Launched=PersistenceCounters().Launched.load();
    const auto WorkspaceBefore=Measure.Mark(true);
    Store->Put(MoveTemp(Huge),FPersistenceGuard()); const auto WorkspaceAfter=Measure.Mark();
    Measure.Record(TEXT("existing_8MiB_JSON_candidate_refusal"),WorkspaceBefore,WorkspaceAfter,WorkspaceAfter,false);
    TestEqual(TEXT("overworkspace candidate refused before worker"),PersistenceCounters().Launched.load(),Launched);
    auto Oversized=Entry(TEXT("over-record")); Oversized.Body.Init(1,13*1024*1024);
    const auto OversizedBefore=Measure.Mark(true);Store->Put(MoveTemp(Oversized),FPersistenceGuard());const auto OversizedAfter=Measure.Mark();
    Measure.Record(TEXT("existing_13MiB_body_candidate_refusal"),OversizedBefore,OversizedAfter,OversizedAfter,false);
    TestEqual(TEXT("oversized record refused before worker"),PersistenceCounters().Launched.load(),Launched);
    // The same immutable read cap backs the worker and its reservation through handback.
    FScopedCacheStoreHooks Hooks(Root); auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->AfterRead=Gate;
    ON_SCOPE_EXIT { Gate->Released=true; Disk.Drain(); };
    const auto HeldReadBefore=Measure.Mark(true);Found=Find(Store,Safe.Key);const auto HeldReadSubmitted=Measure.Mark();
    if (!TestTrue(TEXT("read holds result"),WaitEntered(Gate))) return false;
    if (Measure.Enabled) Measure.Data.Set(TEXT("actual_held_read_logical_bytes"),PersistencePendingBytesForTest());
    TestEqual(TEXT("default read logical reservation"),PersistencePendingBytesForTest(),112LL*1024*1024);
    const auto BudgetBefore=Measure.Mark(true);
    TestFalse(TEXT("independent executor budget prevents another128MiB admission"),LaunchPersistence([]{ return FIoResult(); },[](FIoResult&&){},128LL*1024*1024));
    const auto BudgetAfter=Measure.Mark();Measure.Record(TEXT("128MiB_candidate_while_112MiB_owned"),BudgetBefore,BudgetAfter,BudgetAfter,false);
    const auto ReleaseAt=Measure.Mark(true);Gate->Released=true; if (!TestTrue(TEXT("bounded read release"),Disk.Drain())) return false;
    const auto ReleasedDone=Measure.Mark();Measure.Record(TEXT("held_small_read_includes_explicit_test_barrier"),HeldReadBefore,HeldReadSubmitted,ReleasedDone);
    if (Measure.Enabled) { Measure.Data.Set(TEXT("hold_release_to_owned_drain_ms"),(ReleasedDone.At-ReleaseAt.At)*1000.0);Measure.Data.Set(TEXT("held_read_hit"),Found->Value.IsValid());Measure.Data.Set(TEXT("physical_12MiB_successful_read_measured"),false); }
    TestTrue(TEXT("held safe read returns"),Found->Value.IsValid());
    TestEqual(TEXT("charge retired after closure/result disposal"),PersistencePendingBytesForTest(),int64(0));
    if (Measure.Enabled && !TestTrue(TEXT("write fresh M7 disk report"),Measure.Finish())) return false;
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheControlLimitsTest,"VaRest.Http.CacheStore.ControlLimitsAndStop",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheControlLimitsTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheStoreTestPrivate;
    FVaRestM7DiskMeasure Measure(TEXT("VaRest.Http.CacheStore.ControlLimitsAndStop"),TEXT("CacheStore-ControlLimitsAndStop.json"));
    if (Measure.Enabled && !TestTrue(TEXT("fresh absolute M7 disk report destination"),Measure.Valid())) return false;
    FScopedPersistenceTest Disk;
    {
        FScopedCacheControls Controls;
        for (int32 Index=0;Index<4;++Index)
        {
            { auto Store=FDiskCache::Open(FPaths::Combine(Disk.Root,FString::FromInt(Index)),1024);
              if (!TestTrue(TEXT("registry lifetime drain"),Disk.Drain())) return false; }
        }
        auto Store=FDiskCache::Open(FPaths::Combine(Disk.Root,TEXT("last")),1024);
        if (!TestTrue(TEXT("registry prune drain"),Disk.Drain())) return false;
        TestEqual(TEXT("historical weak registry entries pruned"),CacheRegistryForTest(),1);
        auto TooLong=FDiskCache::Open(FString::ChrN(4097,TEXT('x')),1024);
        TestTrue(TEXT("long root permanently disables isolated controls"),TooLong->IsQuarantined());
        TestEqual(TEXT("no long dirty root retained"),DirtyCacheControlsForTest(),0);
        auto Reopened=FDiskCache::Open(FPaths::Combine(Disk.Root,TEXT("safe")),1024);
        TestTrue(TEXT("fallback affects subsequent ordinary root"),Reopened->IsQuarantined());
        const uint64 Before=PersistenceCounters().Launched.load();
        Reopened->Put(Entry(TEXT("disabled")),FPersistenceGuard()); auto Found=Find(Reopened,Entry(TEXT("disabled")).Key);
        TestEqual(TEXT("fallback schedules no retry IO"),PersistenceCounters().Launched.load(),Before);
        TestFalse(TEXT("fallback always misses"),Found->Value.IsValid());
    }
    {
        FScopedCacheControls Controls;
        TestTrue(TEXT("hold byte budget for root boundary"),LaunchPersistence([] { return FIoResult(); },[](FIoResult&&){},128LL*1024*1024));
        FString LongRoot=FPaths::ConvertRelativePathToFull(Disk.Root)+TEXT("/");
        LongRoot+=FString::ChrN(4096-LongRoot.Len(),TEXT('x'));
        const auto LongBefore=Measure.Mark(true);auto AtLimit=FDiskCache::Open(LongRoot,1024);const auto LongAfter=Measure.Mark();
        Measure.Record(TEXT("4096_character_control_intent"),LongBefore,LongAfter,LongAfter,false);
        TestEqual(TEXT("4096-character intent accepted within control bound"),DirtyCacheControlsForTest(),1);
        auto Ordinary=FDiskCache::Open(FPaths::Combine(Disk.Root,TEXT("boundary-control")),1024);
        TestEqual(TEXT("boundary does not trigger process fallback"),DirtyCacheControlsForTest(),2);
        const FString BeyondRoot=LongRoot+TEXT("x");const auto BeyondBefore=Measure.Mark(true);
        auto Beyond=FDiskCache::Open(BeyondRoot,1024);const auto BeyondAfter=Measure.Mark();
        Measure.Record(TEXT("4097_character_control_fallback"),BeyondBefore,BeyondAfter,BeyondAfter,false);
        if (Measure.Enabled) { Measure.Data.Set(TEXT("root_boundary_chars"),LongRoot.Len());Measure.Data.Set(TEXT("root_boundary_allocated_bytes"),int64(LongRoot.GetAllocatedSize()));Measure.Data.Set(TEXT("long_root_physical_directory_created"),false); }
        TestEqual(TEXT("4097-character root not retained"),DirtyCacheControlsForTest(),2);
        TestTrue(TEXT("overbound triggers fallback"),Beyond->IsQuarantined());
        if (!TestTrue(TEXT("root-boundary held bytes drained"),Disk.Drain())) return false;
    }
    {
        FScopedPersistenceRuntime Runtime;
        FScopedCacheControls Controls;
        // Saturate byte ownership, not production counters. Intent is retained without IO.
        TestTrue(TEXT("reserve entire executor budget"),LaunchPersistence([] { return FIoResult(); },[](FIoResult&&){},128LL*1024*1024));
        if (Measure.Enabled)
        {
            TArray<FString> Roots;Roots.Reserve(4097);
            for (int32 Index=0;Index<4097;++Index) Roots.Add(FPaths::Combine(Disk.Root,FString::Printf(TEXT("dirty-%d"),Index)));
            const auto IntentBefore=Measure.Mark(true);
            for (const FString& Root : Roots) FDiskCache::Open(Root,1024)->Clear();
            const auto IntentAfter=Measure.Mark();Measure.Record(TEXT("existing_4097_control_intent_submissions"),IntentBefore,IntentAfter,IntentAfter,false);
            Measure.Data.Set(TEXT("control_intent_attempts"),Roots.Num());Measure.Data.Set(TEXT("dirty_controls_after_attempts"),DirtyCacheControlsForTest());Measure.Data.Set(TEXT("registry_after_attempts"),CacheRegistryForTest());
        }
        else
        {
        for (int32 Index=0;Index<4097;++Index)
            FDiskCache::Open(FPaths::Combine(Disk.Root,FString::Printf(TEXT("dirty-%d"),Index)),1024)->Clear();
        }
        TestEqual(TEXT("dirty namespace count bounded"),DirtyCacheControlsForTest(),4096);
        TestTrue(TEXT("retained registry bounded"),CacheRegistryForTest()<=4097);
        auto Reopened=FDiskCache::Open(FPaths::Combine(Disk.Root,TEXT("dirty-0")),1024);
        TestTrue(TEXT("cap exhaustion sticky across reopen"),Reopened->IsQuarantined());
        if (!TestTrue(TEXT("saturated worker drained"),Disk.Drain())) return false;
        const uint64 Before=PersistenceCounters().Launched.load();
        Reopened->Clear(); Reopened->Put(Entry(TEXT("disabled")),FPersistenceGuard());
        TestEqual(TEXT("no retries after cap fallback"),PersistenceCounters().Launched.load(),Before);
        Runtime.Stop(); FDiskCache::StopControls(); FDiskCache::StopControls();
        TestEqual(TEXT("Stop releases intent after owned drain"),DirtyCacheControlsForTest(),0);
        TestEqual(TEXT("Stop clears weak registry"),CacheRegistryForTest(),0);
        Reopened->Clear();
        TestEqual(TEXT("no post-Stop retry"),PersistenceCounters().Launched.load(),Before);
    }
    if (Measure.Enabled && !TestTrue(TEXT("write fresh M7 disk report"),Measure.Finish())) return false;
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheWorkerLifetimeTest,"VaRest.Http.CacheStore.WorkerCapacityAndHooks",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheWorkerLifetimeTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheStoreTestPrivate;
    FScopedPersistenceTest Disk;
    const FString Root=FPaths::Combine(Disk.Root,TEXT("Store"));
    auto Value=Entry(TEXT("worker"));
    TSharedPtr<FDiskCache,ESPMode::ThreadSafe> Store=FDiskCache::Open(Root,1024*1024);
    if (!TestTrue(TEXT("initial maintenance"),Disk.Drain())) return false;
    {
        FScopedPersistenceHooks Hooks(Disk.Root);
        auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->BeforeCacheWrite=Gate;
        ON_SCOPE_EXIT { Gate->Released=true; Disk.Drain(); };
        Store->Put(Value,FPersistenceGuard());
        if (!TestTrue(TEXT("actual persistence root hook captured despite canonical child"),WaitEntered(Gate))) return false;
        TWeakPtr<FDiskCache,ESPMode::ThreadSafe> Weak=Store;
        Store.Reset();
        Store=FDiskCache::Open(Root,1);
        TestTrue(TEXT("worker retention preserves store lifetime"),Weak.Pin()==Store);
        auto Larger=FDiskCache::Open(Root,1024*1024);
        TestTrue(TEXT("larger registrant cannot bypass held reduction"),Larger==Store);
        Gate->Released=true;
        if (!TestTrue(TEXT("old write then smaller capacity control"),Disk.Drain())) return false;
        auto Found=Find(Larger,Value.Key); if (!TestTrue(TEXT("reduced result"),Disk.Drain())) return false;
        TestFalse(TEXT("in-flight larger write evicted by ordered reduction"),Found->Value.IsValid());
    }
    Store.Reset(); Store=FDiskCache::Open(Root,1024*1024);
    Store->Put(Value,FPersistenceGuard()); if (!TestTrue(TEXT("fresh lifetime seed"),Disk.Drain())) return false;
    {
        FScopedPersistenceHooks Hooks(Disk.Root); Hooks.Hooks->bFailCacheControl=true;
        Store->Clear(); if (!TestTrue(TEXT("captured failure control"),Disk.Drain())) return false;
        TestTrue(TEXT("existing immutable control hook refuses maintenance"),Store->IsQuarantined());
        auto Found=Find(Store.ToSharedRef(),Value.Key); if (!TestTrue(TEXT("failed control reuse"),Disk.Drain())) return false;
        TestFalse(TEXT("existing control failure keeps miss"),Found->Value.IsValid());
    }
    auto Found=Find(Store.ToSharedRef(),Value.Key); if (!TestTrue(TEXT("retry after hook scope"),Disk.Drain())) return false;
    TestFalse(TEXT("successful clear eventually removes old record"),Found->Value.IsValid());
    TestFalse(TEXT("success releases quarantine"),Store->IsQuarantined());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheGenerationTest,"VaRest.Http.CacheStore.InterruptedGenerations",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheGenerationTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheStoreTestPrivate;
    FScopedPersistenceTest Disk;
    const FString Root=FPaths::Combine(Disk.Root,TEXT("Store"));
    auto Older=Entry(TEXT("generations")); auto Newer=Older; Newer.Generation=FGuid::NewGuid(); Newer.Body={4,5,6};
    const auto Wrote=MakeShared<bool,ESPMode::ThreadSafe>(false);
    LaunchPersistence([Root,Older,Newer,Wrote]
    {
        const FString OldPath=FPaths::Combine(Root,Older.Key+TEXT(".")+Older.Generation.ToString(EGuidFormats::DigitsLower)+TEXT(".json"));
        const FString NewPath=FPaths::Combine(Root,Newer.Key+TEXT(".")+Newer.Generation.ToString(EGuidFormats::DigitsLower)+TEXT(".json"));
        auto Result=WriteNewFile(OldPath,EncodeCacheRecord(Older));
        if (!Result.bOk) return Result;
        Result=WriteNewFile(NewPath,EncodeCacheRecord(Newer)); if (!Result.bOk) return Result;
        FPlatformFileManager::Get().GetPlatformFile().SetTimeStamp(*OldPath,FDateTime(2026,1,1));
        FPlatformFileManager::Get().GetPlatformFile().SetTimeStamp(*NewPath,FDateTime(2027,1,1));
        const FString BadPath=FPaths::Combine(Root,Older.Key+TEXT(".")+FGuid::NewGuid().ToString(EGuidFormats::DigitsLower)+TEXT(".json"));
        Result=WriteNewFile(BadPath,TArray<uint8>{'{'});
        FPlatformFileManager::Get().GetPlatformFile().SetTimeStamp(*BadPath,FDateTime(2028,1,1));
        *Wrote=Result.bOk; return Result;
    },[](FIoResult&&){});
    if (!TestTrue(TEXT("interrupted generations seeded"),Disk.Drain()) || !TestTrue(TEXT("fixture writes"),*Wrote)) return false;
    auto Store=FDiskCache::Open(Root,1024*1024);
    auto Found=Find(Store,Older.Key); if (!TestTrue(TEXT("generation recovery"),Disk.Drain())) return false;
    TestTrue(TEXT("newest valid generation follows corrupt newest"),Found->Value.IsValid() && Found->Value->Body==Newer.Body);
    auto Names=List(Root); if (!TestTrue(TEXT("retirement observation"),Disk.Drain())) return false;
    TestTrue(TEXT("corrupt and superseded generations retired"),Names->bOk && Names->Names.Num()==1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheReadAdmissionTest,"VaRest.Http.CacheStore.ReadAdmission",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheReadAdmissionTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheStoreTestPrivate;
    FScopedPersistenceTest Disk;
    FScopedPersistenceRuntime Runtime;
    FScopedCacheControls Controls;
    const FString Root=FPaths::Combine(Disk.Root,TEXT("Store"));
    auto Store=FDiskCache::Open(Root,1024*1024);
    const auto Value=Entry(TEXT("admission"));
    if (!TestTrue(TEXT("healthy maintenance drained"),Disk.Drain())) return false;
    auto Missing=Find(Store,Value.Key);
    TestTrue(TEXT("ordinary miss read admitted"),Missing->bAccepted);
    TestEqual(TEXT("admitted read does not callback inline"),Missing->Calls,0);
    if (!TestTrue(TEXT("ordinary miss drained"),Disk.Drain())) return false;
    TestEqual(TEXT("ordinary miss callback once"),Missing->Calls,1);
    TestFalse(TEXT("ordinary miss payload"),Missing->Value.IsValid());
    TestTrue(TEXT("saturate real executor byte account"),LaunchPersistence([] { return FIoResult(); },[](FIoResult&&){},128LL*1024*1024));
    auto Refused=Find(Store,Value.Key);
    TestFalse(TEXT("healthy-store scheduling refusal distinguishable"),Refused->bAccepted);
    TestEqual(TEXT("refused read has no callback"),Refused->Calls,0);
    Store->Clear();
    auto Quarantined=Find(Store,Value.Key);
    TestTrue(TEXT("quarantine resolves conservative fallback"),Quarantined->bAccepted);
    TestEqual(TEXT("quarantined fallback receives one null"),Quarantined->Calls,1);
    TestFalse(TEXT("quarantined fallback is miss"),Quarantined->Value.IsValid());
    if (!TestTrue(TEXT("saturation drained"),Disk.Drain())) return false;
    auto Retried=Find(Store,Value.Key); if (!TestTrue(TEXT("control retry drained"),Disk.Drain())) return false;
    TestTrue(TEXT("maintenance retry resolves"),Retried->bAccepted && !Store->IsQuarantined());
    Runtime.Stop();
    auto Stopped=Find(Store,Value.Key);
    TestFalse(TEXT("healthy store read after Stop is refused"),Stopped->bAccepted);
    TestEqual(TEXT("Stop refusal no callback"),Stopped->Calls,0);
    FDiskCache::StopControls();
    auto Disabled=Find(Store,Value.Key);
    TestTrue(TEXT("process control fallback stays conservative miss"),Disabled->bAccepted);
    TestEqual(TEXT("control fallback callback once"),Disabled->Calls,1);
    TestFalse(TEXT("control fallback contains no bytes"),Disabled->Value.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheRootCaseTest,"VaRest.Http.CacheStore.LinuxRootCase",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheRootCaseTest::RunTest(const FString& Parameters)
{
#if PLATFORM_LINUX
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheStoreTestPrivate;
    FVaRestM7DiskMeasure Measure(TEXT("VaRest.Http.CacheStore.LinuxRootCase"),TEXT("CacheStore-LinuxRootCase.json"));
    if (Measure.Enabled && !TestTrue(TEXT("fresh absolute M7 disk report destination"),Measure.Valid())) return false;
    FScopedPersistenceTest Disk;
    FScopedCacheControls Controls;
    const FString Base=FPaths::ConvertRelativePathToFull(Disk.Root);
    const FString RootA=FPaths::Combine(Base,TEXT("CaseRoot")),RootB=FPaths::Combine(Base,TEXT("caseroot"));
    const auto Distinct=MakeShared<bool,ESPMode::ThreadSafe>(false);
    LaunchPersistence([Base,RootA,RootB,Distinct]
    {
        FIoResult Result;
        if (!FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Base)) return Result;
        if (mkdir(TCHAR_TO_UTF8(*RootA),0700)!=0 || mkdir(TCHAR_TO_UTF8(*RootB),0700)!=0) return Result;
        struct stat First,Second;
        Result.bOk=lstat(TCHAR_TO_UTF8(*RootA),&First)==0 && lstat(TCHAR_TO_UTF8(*RootB),&Second)==0;
        *Distinct=Result.bOk && S_ISDIR(First.st_mode) && S_ISDIR(Second.st_mode)
            && (First.st_dev!=Second.st_dev || First.st_ino!=Second.st_ino);
        return Result;
    },[](FIoResult&&){});
    if (!TestTrue(TEXT("directory fixture worker"),Disk.Drain()) || !TestTrue(TEXT("two independently witnessed actual directory inodes"),*Distinct)) return false;
    const auto OpenBefore=Measure.Mark(true);
    TSharedPtr<FDiskCache,ESPMode::ThreadSafe> A=FDiskCache::Open(RootA,1024*1024);
    auto B=FDiskCache::Open(RootB,1024*1024);const auto OpenAfter=Measure.Mark();
    Measure.Record(TEXT("two_witnessed_Linux_root_opens"),OpenBefore,OpenAfter,OpenAfter,false);
    TestTrue(TEXT("Linux case-distinct roots have distinct stores"),A!=B);
    TestTrue(TEXT("exact root shares live store"),FDiskCache::Open(RootA,1024*1024)==A);
    auto First=Entry(TEXT("same-key")),Second=First; First.Body={1,2,3}; Second.Body={9,8,7};
    const auto PutBefore=Measure.Mark(true);A->Put(First,FPersistenceGuard()); B->Put(Second,FPersistenceGuard());const auto PutSubmitted=Measure.Mark();
    if (!TestTrue(TEXT("independent writes"),Disk.Drain())) return false;
    Measure.Record(TEXT("two_root_puts_and_existing_maintenance"),PutBefore,PutSubmitted,Measure.Mark());
    const auto ReadsBefore=Measure.Mark(true);auto ReadA=Find(A.ToSharedRef(),First.Key),ReadB=Find(B,Second.Key);const auto ReadsSubmitted=Measure.Mark();
    if (!TestTrue(TEXT("independent reads"),Disk.Drain())) return false;
    Measure.Record(TEXT("existing_two_root_reads_1MiB_policy"),ReadsBefore,ReadsSubmitted,Measure.Mark());
    if (Measure.Enabled) { Measure.Data.Set(TEXT("actual_distinct_directory_witness"),*Distinct);Measure.Data.Set(TEXT("two_root_hits"),int32(ReadA->Value.IsValid())+int32(ReadB->Value.IsValid()));Measure.Data.Set(TEXT("root_test_policy_bytes"),1024*1024); }
    TestTrue(TEXT("first root owns first bytes"),ReadA->Value.IsValid() && ReadA->Value->Body==First.Body);
    TestTrue(TEXT("second root owns second bytes"),ReadB->Value.IsValid() && ReadB->Value->Body==Second.Body);
    {
        FScopedCacheStoreHooks Hooks(RootA); Hooks.Hooks->bFailList=true;
        const auto ClearBefore=Measure.Mark(true);A->Clear();const auto ClearSubmitted=Measure.Mark();
        if (!TestTrue(TEXT("first root failed clear"),Disk.Drain())) return false;
        Measure.Record(TEXT("one_root_clear_with_existing_list_failure_hook"),ClearBefore,ClearSubmitted,Measure.Mark());
        TestTrue(TEXT("first root quarantined"),A->IsQuarantined());
        TestFalse(TEXT("second root not quarantined"),B->IsQuarantined());
        const auto DirtyReopenBefore=Measure.Mark(true);A.Reset(); A=FDiskCache::Open(RootA,1024*1024);
        ReadB=Find(B,Second.Key);const auto DirtyReopenSubmitted=Measure.Mark();
        if (!TestTrue(TEXT("reopened dirty root and independent other"),Disk.Drain())) return false;
        Measure.Record(TEXT("reopen_dirty_root_and_other_root_find"),DirtyReopenBefore,DirtyReopenSubmitted,Measure.Mark());
        TestTrue(TEXT("other root usable while first control fails"),ReadB->Value.IsValid() && ReadB->Value->Body==Second.Body);
    }
    const auto RetryBefore=Measure.Mark(true);ReadA=Find(A.ToSharedRef(),First.Key);const auto RetrySubmitted=Measure.Mark();
    if (!TestTrue(TEXT("first root clear retry"),Disk.Drain())) return false;
    Measure.Record(TEXT("clear_retry_and_find"),RetryBefore,RetrySubmitted,Measure.Mark());
    TestFalse(TEXT("first root cleared"),ReadA->Value.IsValid());
    const auto FinalReopenBefore=Measure.Mark(true);A.Reset(); A=FDiskCache::Open(RootA,1024*1024);
    ReadA=Find(A.ToSharedRef(),First.Key); ReadB=Find(B,Second.Key);const auto FinalReopenSubmitted=Measure.Mark();
    if (!TestTrue(TEXT("complete reopen observation"),Disk.Drain())) return false;
    Measure.Record(TEXT("reopened_cleared_root_and_other_root_find"),FinalReopenBefore,FinalReopenSubmitted,Measure.Mark());
    TestFalse(TEXT("first stays empty after complete reopen"),ReadA->Value.IsValid());
    TestTrue(TEXT("other root bytes survive clear release and reopen"),ReadB->Value.IsValid() && ReadB->Value->Body==Second.Body);
    if (Measure.Enabled) { Measure.Data.Set(TEXT("other_root_hit_after_clear_reopen"),ReadB->Value.IsValid() && ReadB->Value->Body==Second.Body);Measure.Data.Set(TEXT("cleared_root_empty_after_reopen"),!ReadA->Value.IsValid()); }
    if (Measure.Enabled && !TestTrue(TEXT("write fresh M7 disk report"),Measure.Finish())) return false;
#endif
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheSpecialFilesTest,"VaRest.Http.CacheStore.SpecialFiles",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheSpecialFilesTest::RunTest(const FString& Parameters)
{
#if PLATFORM_LINUX
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheStoreTestPrivate;
    FScopedPersistenceTest Disk;
    FScopedCacheControls Controls;
    const FString Root=FPaths::Combine(FPaths::ConvertRelativePathToFull(Disk.Root),TEXT("Store"));
    const auto Value=Entry(TEXT("fifo"));
    const FString Path=FPaths::Combine(Root,Value.Key+TEXT(".")+Value.Generation.ToString(EGuidFormats::DigitsLower)+TEXT(".json"));
    struct FFixture { int Descriptor=-1; bool Witness=false,Listed=false,Preserved=false; };
    const auto Fixture=MakeShared<FFixture,ESPMode::ThreadSafe>();
    ON_SCOPE_EXIT
    {
        LaunchPersistence([Path,Fixture] { if (Fixture->Descriptor>=0) close(Fixture->Descriptor); unlink(TCHAR_TO_UTF8(*Path)); return FIoResult(); },[](FIoResult&&){});
        Disk.Drain();
    };
    LaunchPersistence([Root,Path,Fixture]
    {
        FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Root);
        struct stat Info;
        Fixture->Witness=mkfifo(TCHAR_TO_UTF8(*Path),0600)==0 && lstat(TCHAR_TO_UTF8(*Path),&Info)==0 && S_ISFIFO(Info.st_mode);
        if (!Fixture->Witness) return FIoResult();
        Fixture->Descriptor=open(TCHAR_TO_UTF8(*Path),O_RDWR|O_NONBLOCK|O_CLOEXEC);
        Fixture->Witness=Fixture->Descriptor>=0;
        const auto Listed=ListFiles(Root); Fixture->Listed=Listed.bOk && Listed.Names.IsEmpty();
        return FIoResult();
    },[](FIoResult&&){});
    if (!TestTrue(TEXT("bounded FIFO fixture drain"),Disk.Drain()) || !TestTrue(TEXT("actual FIFO held open without blocking"),Fixture->Witness)) return false;
    auto Store=FDiskCache::Open(Root,1024*1024);
    if (!TestTrue(TEXT("initial FIFO namespace maintenance"),Disk.Drain())) return false;
    auto Found=Find(Store,Value.Key);
    Store->Clear();
    if (!TestTrue(TEXT("lookup and control drain after special node"),Disk.Drain())) return false;
    TestTrue(TEXT("special cache leaf excluded by listing"),Fixture->Listed);
    TestFalse(TEXT("special cache leaf is a miss"),Found->Value.IsValid());
    TestFalse(TEXT("ignored special node does not quarantine healthy namespace"),Store->IsQuarantined());
    LaunchPersistence([Path,Fixture] { struct stat Info; Fixture->Preserved=lstat(TCHAR_TO_UTF8(*Path),&Info)==0 && S_ISFIFO(Info.st_mode); return FIoResult(); },[](FIoResult&&){});
    Store->Put(Value,FPersistenceGuard());
    if (!TestTrue(TEXT("regular put after FIFO"),Disk.Drain())) return false;
    Found=Find(Store,Value.Key); if (!TestTrue(TEXT("regular read after FIFO"),Disk.Drain())) return false;
    TestTrue(TEXT("special cache node preserved"),Fixture->Preserved);
    TestTrue(TEXT("ordinary record works alongside special node"),Found->Value.IsValid() && Found->Value->Body==Value.Body);
#endif
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCachePathErrorsTest,"VaRest.Http.CacheStore.PathErrors",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCachePathErrorsTest::RunTest(const FString& Parameters)
{
#if PLATFORM_LINUX
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheStoreTestPrivate;
    FScopedPersistenceTest Disk;
    FScopedCacheControls Controls;
    for (bool bRootError : {true,false}) for (bool bCapacity : {false,true})
    {
        const FString Parent=FPaths::Combine(FPaths::ConvertRelativePathToFull(Disk.Root),FString::Printf(TEXT("parent-%d-%d"),bRootError,bCapacity));
        const FString Root=FPaths::Combine(Parent,TEXT("namespace"));
        ON_SCOPE_EXIT
        {
            LaunchPersistence([Parent,Root] { chmod(TCHAR_TO_UTF8(*Parent),0700); chmod(TCHAR_TO_UTF8(*Root),0700); return FIoResult(); },[](FIoResult&&){});
            Disk.Drain();
        };
        TSharedPtr<FDiskCache,ESPMode::ThreadSafe> Store=FDiskCache::Open(Root,1024*1024);
        const auto Value=Entry(TEXT("permission"));
        Store->Put(Value,FPersistenceGuard()); if (!TestTrue(TEXT("permission seed"),Disk.Drain())) return false;
        const auto Witness=MakeShared<bool,ESPMode::ThreadSafe>(false);
        LaunchPersistence([Parent,Root,bRootError,Witness]
        {
            const auto Names=ListFiles(Root); if (!Names.bOk || Names.Names.Num()!=1) return FIoResult();
            const FString File=FPaths::Combine(Root,Names.Names[0]);
            const FString Changed=bRootError ? Parent : Root;
            if (geteuid()==0 || chmod(TCHAR_TO_UTF8(*Changed),0400)!=0) return FIoResult();
            struct stat Info; errno=0;
            const FString Probe=bRootError ? Root : File;
            *Witness=lstat(TCHAR_TO_UTF8(*Probe),&Info)==-1 && errno==EACCES;
            if (!bRootError)
            {
                DIR* Directory=opendir(TCHAR_TO_UTF8(*Root)); bool Found=false;
                if (Directory) { while (const dirent* Item=readdir(Directory)) if (FString(UTF8_TO_TCHAR(Item->d_name))==Names.Names[0]) Found=true; closedir(Directory); }
                *Witness&=Found; // Native enumeration succeeds while native child stat fails.
            }
            return FIoResult();
        },[](FIoResult&&){});
        if (!TestTrue(TEXT("permission witness drain"),Disk.Drain()) || !TestTrue(TEXT("actual EACCES route independently witnessed"),*Witness)) return false;
        if (bCapacity) Store=FDiskCache::Open(Root,1); else Store->Clear();
        if (!TestTrue(TEXT("failed mandatory control drain"),Disk.Drain())) return false;
        TestTrue(TEXT("unknown filesystem keeps quarantine"),Store->IsQuarantined());
        Store.Reset(); TestEqual(TEXT("failed control retained without holder"),DirtyCacheControlsForTest(),1);
        Store=FDiskCache::Open(Root,bCapacity ? 1 : 1024*1024);
        auto Found=Find(Store.ToSharedRef(),Value.Key);
        if (!TestTrue(TEXT("inaccessible reopen drains"),Disk.Drain())) return false;
        TestTrue(TEXT("unknown filesystem stays quarantined after reopen"),Store->IsQuarantined());
        TestFalse(TEXT("no stale reuse while inaccessible"),Found->Value.IsValid());
        LaunchPersistence([Parent,Root] { chmod(TCHAR_TO_UTF8(*Parent),0700); chmod(TCHAR_TO_UTF8(*Root),0700); return FIoResult(); },[](FIoResult&&){});
        if (!TestTrue(TEXT("restore permissions"),Disk.Drain())) return false;
        Found=Find(Store.ToSharedRef(),Value.Key);
        if (!TestTrue(TEXT("successful mandatory retry"),Disk.Drain())) return false;
        TestFalse(TEXT("old bytes never resurrect after access returns"),Found->Value.IsValid());
        TestFalse(TEXT("successful retry clears quarantine"),Store->IsQuarantined());
        TestEqual(TEXT("successful retry retires dirty intent"),DirtyCacheControlsForTest(),0);
        Store.Reset(); if (!TestTrue(TEXT("complete release"),Disk.Drain())) return false;
        Store=FDiskCache::Open(Root,1024*1024); Store->Put(Value,FPersistenceGuard());
        if (!TestTrue(TEXT("healthy replacement write"),Disk.Drain())) return false;
        Found=Find(Store.ToSharedRef(),Value.Key); if (!TestTrue(TEXT("healthy replacement read"),Disk.Drain())) return false;
        TestTrue(TEXT("ordinary storage works after access recovery"),Found->Value.IsValid() && Found->Value->Body==Value.Body);
    }
#endif
    return true;
}
#endif
