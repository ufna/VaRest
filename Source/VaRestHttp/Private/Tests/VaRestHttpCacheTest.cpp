// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS
#include "VaRestHttpTesting.h"
#include "VaRestCache.h"
#include "VaRestAuth.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "VaRestHttpSettings.h"
#include "VaRestClientState.h"
#include "VaRestDiskCache.h"
#include "VaRestIdentity.h"
#include "VaRestPersistence.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheRefreshAndAccountsTest,"VaRest.Http.Cache.RefreshAndAccounts",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheRefreshAndAccountsTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FTestRig Rig;
auto Policy = FVaRestCachePolicy::Memory();
Policy.DefaultTtlSeconds = 60;
Rig.Client.WithCache(Policy).WithAuth(FVaRestAuth::Bearer());
Rig.Client.SetBearerToken(TEXT("A-token-1234"));
Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200, TEXT("A-body")));
auto First = MakeOutcome<FString>();
Observe(Rig.Client.Get<FString>(TEXT("/me")), First).Send();
if (!TestTrue(TEXT("first"), Rig.Wait(First))) return false;
Rig.Client.SetBearerToken(TEXT("A-token-rotated"), true);
auto Second = MakeOutcome<FString>();
Observe(Rig.Client.Get<FString>(TEXT("/me")), Second).Send();
if (!TestTrue(TEXT("second"), Rig.Wait(Second))) return false;
TestEqual(TEXT("refresh remains cache hit"), Rig.Fake->NumCalls(), 1);
TestTrue(TEXT("response provenance"), Second->Response.bFromCache);
if (!TestTrue(TEXT("value present"), Second->Value.IsSet())) return false;
TestEqual(TEXT("bytes decoded each time"), Second->Value.GetValue(), FString(TEXT("A-body")));
Rig.Client.SetBearerToken(TEXT("B-token-1234"));
Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200, TEXT("B-body")));
auto Third = MakeOutcome<FString>();
Observe(Rig.Client.Get<FString>(TEXT("/me")), Third).Send();
if (!TestTrue(TEXT("third"), Rig.Wait(Third))) return false;
TestEqual(TEXT("other account sent"), Rig.Fake->NumCalls(), 2);
TestFalse(TEXT("other account not cached A"), Third->Response.bFromCache);
TestTrue(TEXT("drains"), Rig.Drain());
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheHead304Test,"VaRest.Http.Cache.Head304",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheHead304Test::RunTest(const FString& Parameters)
{
using namespace VaRest::Http::Testing;
FTestRig Rig;
Rig.Client.WithCache(FVaRestCachePolicy::Memory());
FVaRestFakeResponse First=FVaRestFakeResponse::Text(200,TEXT(""));
First.Headers.Set(TEXT("Cache-Control"),TEXT("max-age=0"));
First.Headers.Set(TEXT("ETag"),TEXT("\"head-v1\""));
First.Headers.Set(TEXT("Content-Length"),TEXT("7123"));
Rig.Fake->Enqueue(MoveTemp(First));
auto A=MakeOutcome<void>(); Observe(Rig.Client.Head<void>(TEXT("/head")),A).Send();
if (!TestTrue(TEXT("initial HEAD"),Rig.Wait(A))) return false;
FVaRestFakeResponse Second=FVaRestFakeResponse::Text(304,TEXT(""));
Second.Headers.Set(TEXT("Cache-Control"),TEXT("max-age=60"));
Rig.Fake->Enqueue(MoveTemp(Second));
auto B=MakeOutcome<void>(); Observe(Rig.Client.Head<void>(TEXT("/head")),B).Send();
if (!TestTrue(TEXT("revalidated HEAD"),Rig.Wait(B))) return false;
if (!TestEqual(TEXT("two physical calls"),Rig.Fake->NumCalls(),2)) return false;
TestEqual(TEXT("validator sent"),Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("If-None-Match")),FString(TEXT("\"head-v1\"")));
TestTrue(TEXT("304 returns cache representation"),B->Response.bFromCache);
TestEqual(TEXT("HEAD length preserved"),B->Response.Headers.Get(TEXT("Content-Length")),FString(TEXT("7123")));
auto C=MakeOutcome<void>(); Observe(Rig.Client.Head<void>(TEXT("/head")),C).Send();
if (!TestTrue(TEXT("fresh HEAD"),Rig.Wait(C))) return false;
TestEqual(TEXT("fresh hit makes no wire call"),Rig.Fake->NumCalls(),2);
TestTrue(TEXT("fresh response from cache"),C->Response.bFromCache);
TestEqual(TEXT("fresh HEAD length preserved"),C->Response.Headers.Get(TEXT("Content-Length")),FString(TEXT("7123")));
TestTrue(TEXT("drained"),Rig.Drain());
return true;
}

namespace VaRest::Http::CacheTestPrivate
{
    FVaRestFakeResponse Response(int32 Status=200, const TCHAR* Body=TEXT("original"), const TCHAR* Control=TEXT("max-age=60"))
    {
        auto Result=FVaRestFakeResponse::Text(Status,Body);
        Result.Headers.Set(TEXT("Cache-Control"),Control);
        Result.Headers.Set(TEXT("ETag"),TEXT("\"one\""));
        return Result;
    }
    struct FCleanup
    {
        explicit FCleanup(Testing::FTestRig& InRig) : Rig(InRig) {}
        ~FCleanup() { Rig.Client.CancelAll(); Rig.Drain(); }
        Testing::FTestRig& Rig;
    };
    FString Directory(const Testing::FTestRig& Rig)
    {
        const auto Config=FClientAccess::State(Rig.Client)->GetConfig();
        FString Origin; CanonicalOrigin(Config->BaseUrl,Origin);
        return FPaths::Combine(Config->CacheRoot,HashFields(TEXT("VaRest/cache-origin/v1"),TArray<FString>{Origin}));
    }
    // Captures only test-owned weak exchange state; optionally delays entry for live-time checks.
    class FCapture final : public IVaRestStage
    {
    public:
        virtual FName GetStageName() const override { return TEXT("CacheCapture"); }
        virtual void Process(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done) override
        {
            Seen=Exchange; bPendingAtEntry=Internals(*Exchange).bCacheLookupPending;
            if (bHold) Pending=[Exchange,Next,Callback=MoveTemp(Done)]() mutable { Next.Proceed(Exchange,MoveTemp(Callback)); };
            else Next.Proceed(Exchange,MoveTemp(Done));
        }
        void Release() { auto Callback=MoveTemp(Pending); if (Callback) Callback(); }
        TWeakPtr<FVaRestExchange,ESPMode::ThreadSafe> Seen;
        TUniqueFunction<void()> Pending;
        bool bHold=false;
        bool bPendingAtEntry=false;
    };
    class FHeldClock final : public IVaRestClock
    {
    public:
        virtual double Now() const override { return Seconds; }
        virtual FVaRestTimerHandle SetTimer(double DelaySeconds,TUniqueFunction<void()>&& Callback) override
        { const uint64 Id=NewTimerId(); Pending.Add(Id,MoveTemp(Callback)); return {Id}; }
        virtual void ClearTimer(FVaRestTimerHandle& Handle) override { Pending.Remove(Handle.Id); Handle.Invalidate(); }
        double Seconds=1;
        TMap<uint64,TUniqueFunction<void()>> Pending;
    };
    class FUnknownAuth final : public IVaRestAuth
    {
    public:
        virtual FName GetAuthName() const override { return TEXT("Unknown"); }
        virtual bool Apply(FVaRestAuthTarget& Target) const override { return true; }
    };
    int32 Notes(const Testing::FTraceRecorder& Recorder,uint64 Id,EVaRestTraceNoteKind Kind)
    {
        int32 Count=0;
        for (const auto& Event : Recorder.For(Id))
            if (Event.Kind==EVaRestTraceKind::Note && Event.Payload.Get<FVaRestTraceNote>().Kind==Kind) ++Count;
        return Count;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheRevalidateTest,"VaRest.Http.Cache.Revalidate304",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheRevalidateTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheTestPrivate;
    FTestRig Rig; FCleanup Cleanup(Rig); FScopedTraceRecorder Trace;
    Rig.Client.WithCache(FVaRestCachePolicy::Memory());
    Rig.Fake->Enqueue(Response(200,TEXT("original"),TEXT("max-age=1")));
    auto A=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/cached")),A).Send();
    if (!TestTrue(TEXT("first"),Rig.Wait(A))) return false;
    Rig.Clock->Advance(1);
    Rig.Fake->Enqueue(Response(304,TEXT(""),TEXT("max-age=20")));
    auto B=MakeOutcome<FString>(); auto H=Observe(Rig.Client.Get<FString>(TEXT("/cached")),B).Send();
    if (!TestTrue(TEXT("second"),Rig.Wait(B)) || !TestEqual(TEXT("two calls"),Rig.Fake->NumCalls(),2)) return false;
    TestEqual(TEXT("ETag sent"),Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("If-None-Match")),FString(TEXT("\"one\"")));
    TestTrue(TEXT("200 cached"),B->Response.Status==200 && B->Response.bFromCache);
    if (!TestTrue(TEXT("value present"),B->Value.IsSet())) return false;
    TestEqual(TEXT("original bytes"),B->Value.GetValue(),FString(TEXT("original")));
    TestEqual(TEXT("GET length repaired"),B->Response.Headers.Get(TEXT("Content-Length")),FString(TEXT("8")));
    TestEqual(TEXT("one revalidation note"),Notes(*Trace.Recorder,H.GetId(),EVaRestTraceNoteKind::CacheRevalidated),1);
    auto C=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/cached")),C).Send();
    TestFalse(TEXT("hit noninline"),C->IsDone());
    if (!TestTrue(TEXT("third"),Rig.Wait(C))) return false;
    TestEqual(TEXT("fresh no wire"),Rig.Fake->NumCalls(),2);
    TestTrue(TEXT("fresh provenance"),C->Response.bFromCache);
    const auto State=FClientAccess::State(Rig.Client);
    TestEqual(TEXT("one lookup per request"),State->GetCounters().CacheLookups.load(),int64(3));
    TestEqual(TEXT("one hit per cache answer"),State->GetCounters().CacheHits.load(),int64(2));
    TestEqual(TEXT("cached bytes do not increase network download"),State->GetCounters().BytesDown.load(),int64(8));
    TestTrue(TEXT("drained"),Rig.Drain()); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheNoStoreTest,"VaRest.Http.Cache.NoCacheNoStore",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheNoStoreTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheTestPrivate;
    for (int32 Mode=0;Mode<8;++Mode)
    {
        FTestRig Rig; FCleanup Cleanup(Rig);
        auto Policy=FVaRestCachePolicy::Memory(); Policy.DefaultTtlSeconds=60; Rig.Client.WithCache(Policy);
        for (int32 Index=0;Index<3;++Index)
        {
            auto R=Response(Mode==0 && Index>0 ? 304 : Mode==3 ? 206 : Mode==4 ? 302 : 200,TEXT("original"),Mode==0 ? TEXT("no-cache") : Mode==1 ? TEXT("no-store") : TEXT("max-age=60"));
            if (Mode==5) R.Headers.Set(TEXT("Set-Cookie"),TEXT("secret=session"));
            if (Mode==6) R.Headers.Set(TEXT("Vary"),TEXT("X-Transport-Only"));
            if (Mode==7) R.Headers.Set(TEXT("Vary"),TEXT("*"));
            Rig.Fake->Enqueue(MoveTemp(R));
            auto A=MakeOutcome<FString>(); auto Request=Rig.Client.Get<FString>(TEXT("/policy"));
            if (Mode==2) Request=MoveTemp(Request).NoCache();
            Observe(MoveTemp(Request),A).Send();
            if (!TestTrue(TEXT("policy response"),Rig.Wait(A))) return false;
            TestEqual(TEXT("every use needs wire"),Rig.Fake->NumCalls(),Index+1);
            TestEqual(TEXT("only 304 yields cached response"),A->Response.bFromCache,Mode==0 && Index>0);
        }
        const auto State=FClientAccess::State(Rig.Client);
        TestEqual(TEXT("disabled has no lookup stats"),State->GetCounters().CacheLookups.load(),Mode==2 ? int64(0) : int64(3));
        TestTrue(TEXT("drained"),Rig.Drain());
    }
    FTestRig Rig; FCleanup Cleanup(Rig); Rig.Client.WithCache(FVaRestCachePolicy::Memory());
    Rig.Fake->Enqueue(Response()); Rig.Fake->Enqueue(Response(200,TEXT("")));
    auto A=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/same")),A).Send(); if (!TestTrue(TEXT("GET"),Rig.Wait(A))) return false;
    auto B=MakeOutcome<void>(); Observe(Rig.Client.Head<void>(TEXT("/same")),B).Send(); if (!TestTrue(TEXT("HEAD"),Rig.Wait(B))) return false;
    auto C=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/same")),C).Send(); if (!TestTrue(TEXT("GET again"),Rig.Wait(C))) return false;
    TestEqual(TEXT("GET and HEAD distinct"),Rig.Fake->NumCalls(),2);
    if (!TestTrue(TEXT("GET kept bytes"),C->Value.IsSet())) return false;
    TestEqual(TEXT("HEAD cannot replace GET"),C->Value.GetValue(),FString(TEXT("original")));
    return TestTrue(TEXT("drained"),Rig.Drain());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheBypassTest,"VaRest.Http.Cache.EligibilityAndJsonCap",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheBypassTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheTestPrivate;
    for (int32 Mode=0;Mode<10;++Mode)
    {
        FTestRig Rig; FCleanup Cleanup(Rig); FScopedPersistenceTest Disk;
        Rig.Client.WithCache(FVaRestCachePolicy::Memory());
        if (Mode==0) Rig.Client.WithAuth(MakeShared<FUnknownAuth,ESPMode::ThreadSafe>());
        if (Mode==1) Rig.Client.Use(MakeShared<FCapture,ESPMode::ThreadSafe>(),EVaRestStageSlot::PerAttempt);
        for (int32 Index=0;Index<2;++Index)
        {
            Rig.Fake->Enqueue(Response()); auto A=MakeOutcome<void>(); auto Request=Mode==8 ? Rig.Client.Head<void>(TEXT("/bypass")) : Rig.Client.Get<void>(TEXT("/bypass"));
            if (Mode==2 || Mode==8) Request=MoveTemp(Request).Body(TEXT("body"));
            if (Mode==3) Request=MoveTemp(Request).SaveTo(FPaths::Combine(Disk.Root,TEXT("download.txt")));
            if (Mode==4) Request=MoveTemp(Request).Header(TEXT("Range"),TEXT("bytes=0-1"));
            if (Mode==5) Request=MoveTemp(Request).Header(TEXT("If-None-Match"),TEXT("caller"));
            if (Mode==6) Request=MoveTemp(Request).Header(TEXT("If-Modified-Since"),TEXT("caller"));
            if (Mode==7) Request=MoveTemp(Request).Header(TEXT("Cache-Control"),TEXT("no-store"));
            if (Mode==9) Request=MoveTemp(Request).Header(TEXT("If-Range"),TEXT("caller"));
            Observe(MoveTemp(Request),A).Send(); if (!TestTrue(TEXT("bypass completes"),Rig.Wait(A))) return false;
            TestFalse(TEXT("not cache"),A->Response.bFromCache);
            if (Mode==8) TestEqual(TEXT("existing HEAD body rejection"),A->Error.Kind,EVaRestErrorKind::Config);
        }
        TestEqual(TEXT("eligible wire semantics preserved"),Rig.Fake->NumCalls(),Mode==8 ? 0 : 2);
        TestEqual(TEXT("ineligible has no lookup"),FClientAccess::State(Rig.Client)->GetCounters().CacheLookups.load(),int64(0));
        TestTrue(TEXT("drain"),Rig.Drain() && Disk.Drain());
    }
    FTestRig Rig; FCleanup Cleanup(Rig); auto Policy=FVaRestCachePolicy::Memory(); Policy.DefaultTtlSeconds=60; Rig.Client.WithCache(Policy);
    for (int32 Index=0;Index<2;++Index) Rig.Fake->Enqueue(FVaRestFakeResponse::Json(200,UTF8TEXT("{\"data\":\"a long body\"}")));
    auto A=MakeOutcome<FVaRestJson>(); Observe(Rig.Client.Get<FVaRestJson>(TEXT("/json")),A).Send(); if (!TestTrue(TEXT("seed JSON"),Rig.Wait(A))) return false;
    auto B=MakeOutcome<FVaRestJson>(); Observe(Rig.Client.Get<FVaRestJson>(TEXT("/json")).MaxJsonBytes(4),B).Send(); if (!TestTrue(TEXT("limited JSON"),Rig.Wait(B))) return false;
    TestEqual(TEXT("lower cap goes to normal bounded transport"),Rig.Fake->NumCalls(),2);
    TestEqual(TEXT("cap does not decode cached oversized bytes"),B->SuccessCalls.load(),0);
    TestFalse(TEXT("not cached"),B->Response.bFromCache);
    return TestTrue(TEXT("drain"),Rig.Drain());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheOfflineTest,"VaRest.Http.Cache.OfflineAndStale",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheOfflineTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheTestPrivate;
    for (int32 Mode=0;Mode<10;++Mode)
    {
        FTestRig Rig; FCleanup Cleanup(Rig); FScopedTraceRecorder Trace;
        auto Policy=FVaRestCachePolicy::Memory(); Policy.bServeStaleOnError=Mode!=3; Rig.Client.WithCache(Policy);
        Rig.Fake->Enqueue(Response(200,TEXT("stale"),Mode==4 ? TEXT("max-age=1,must-revalidate") : Mode==5 ? TEXT("no-cache") : TEXT("max-age=1")));
        auto A=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/offline")),A).Send(); if (!TestTrue(TEXT("seed"),Rig.Wait(A))) return false;
        if (Mode!=0) Rig.Clock->Advance(1);
        if (Mode<6) Rig.Connectivity->ReportOffline();
        else Rig.Fake->Enqueue(Mode==6 ? FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection) : Mode==7 ? FVaRestFakeResponse::Failed(EVaRestTransportFailure::Timeout) : Response(Mode==8 ? 500 : 401));
        auto B=MakeOutcome<FString>(); auto Request=Rig.Client.Get<FString>(TEXT("/offline"));
        if (Mode==2) Request=MoveTemp(Request).NoCache();
        auto H=Observe(MoveTemp(Request),B).Send(); TestFalse(TEXT("noninline"),B->IsDone());
        if (!TestTrue(TEXT("second"),Rig.Wait(B))) return false;
        const bool Hit=Mode==0 || Mode==1 || Mode==6;
        TestEqual(TEXT("only eligible stale/fresh answer"),B->Response.bFromCache,Hit);
        TestEqual(TEXT("offline never wire"),Rig.Fake->NumCalls(),Mode<6 ? 1 : 2);
        if (Mode<6 && !Hit) TestEqual(TEXT("miss Offline"),B->Error.Kind,EVaRestErrorKind::Offline);
        if (Mode==7) TestEqual(TEXT("timeout not hidden"),B->Error.Kind,EVaRestErrorKind::Timeout);
        if (Mode>=8) TestEqual(TEXT("HTTP not hidden"),B->Error.Kind,EVaRestErrorKind::Http);
        TestEqual(TEXT("stale note"),Notes(*Trace.Recorder,H.GetId(),EVaRestTraceNoteKind::CacheServedStale),(Mode==1 || Mode==6) ? 1 : 0);
        if (Mode<=6) TestFalse(TEXT("cache cannot establish connectivity"),Rig.Connectivity->IsOnline());
        TestTrue(TEXT("drain"),Rig.Drain());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheDiskLifecycleTest,"VaRest.Http.Cache.DiskLifecycle",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheDiskLifecycleTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::CacheTestPrivate;
    // Each row holds an actual fresh read. Account replacement is the joint key/identity mutation witness.
    for (int32 Mode=0;Mode<5;++Mode)
    {
        FScopedPersistenceTest Disk; FScopedCacheControls Controls; FTestRig Rig; FCleanup Cleanup(Rig);
        auto Policy=FVaRestCachePolicy::MemoryAndDisk(); Policy.DefaultTtlSeconds=60;
        Rig.Client.WithStorageDirectories(Disk.Root,Disk.Root).WithCache(Policy).WithAuth(FVaRestAuth::Bearer());
        Rig.Client.SetBearerToken(TEXT("account-A-token"));
        const auto State=FClientAccess::State(Rig.Client);
        const auto Probe=MakeShared<FOutcomeProbe,ESPMode::ThreadSafe>(); const auto Capture=MakeShared<FCapture,ESPMode::ThreadSafe>();
        Rig.Client.Use(Probe,EVaRestStageSlot::Outer).Use(Capture,EVaRestStageSlot::Outer);
        Rig.Fake->Enqueue(Response(200,TEXT("A-bytes")));
        auto A=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/disk")),A).Send();
        if (!TestTrue(TEXT("seed disk read and write"),Disk.Drain()) || !TestTrue(TEXT("seed response"),Rig.Wait(A)) || !TestTrue(TEXT("seed durable"),Disk.Drain())) return false;
        State->GetMemoryCache().Clear();
        FScopedCacheStoreHooks Hooks(Directory(Rig));
        auto Barrier=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->AfterRead=Barrier;
        ON_SCOPE_EXIT { Barrier->Released=true; Disk.Drain(); Rig.Client.CancelAll(); Rig.Drain(); };
        const auto HeldClock=MakeShared<FHeldClock,ESPMode::ThreadSafe>();
        if (Mode==3) Rig.Client.WithClock(HeldClock);
        auto B=MakeOutcome<FString>(); auto Request=Rig.Client.Get<FString>(TEXT("/disk"));
        UObject* Owner=nullptr;
        if (Mode==3) Request=MoveTemp(Request).Retry(FVaRestRetryPolicy::Disabled().WithDeadline(1));
        if (Mode==4) { Owner=NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient); Request=MoveTemp(Request).Owner(Owner); }
        auto H=Observe(MoveTemp(Request),B).Send();
        if (!TestTrue(TEXT("actual disk read held after bytes"),PumpUntil([Barrier] { return Barrier->Entered.load(); }))) return false;
        TestFalse(TEXT("read unresolved"),B->IsDone());
        if (Mode==0) H.Cancel();
        if (Mode==1) Rig.Client.ClearCache();
        if (Mode==2) Rig.Client.SetBearerToken(TEXT("account-B-token"));
        if (Mode==3) HeldClock->Seconds+=1;
        if (Mode==4) Owner->MarkAsGarbage();
        Rig.Fake->Enqueue(Response(200,TEXT("B-wire")));
        Barrier->Released=true;
        if (!TestTrue(TEXT("held read drains"),Disk.Drain()) || !TestTrue(TEXT("owned request settles"),PumpUntil([H] { return !H.IsActive(); }))) return false;
        if (Mode!=4 && !TestTrue(TEXT("typed result"),Rig.Wait(B))) return false;
        TestFalse(TEXT("never late cached A"),B->Response.bFromCache);
        TestEqual(TEXT("outer exactly once"),Probe->Total(H.GetId()),1);
        if (Mode==0) TestEqual(TEXT("cancel result"),B->Error.Kind,EVaRestErrorKind::Canceled);
        if (Mode==3) TestEqual(TEXT("actual Now deadline"),B->Error.Kind,EVaRestErrorKind::Timeout);
        if (Mode==4) TestEqual(TEXT("owner callbacks silent"),B->CompleteCalls.load(),0);
        if (Mode==1 || Mode==2)
        {
            if (!TestTrue(TEXT("wire body present"),B->Value.IsSet())) return false;
            TestEqual(TEXT("only current wire bytes"),B->Value.GetValue(),FString(TEXT("B-wire")));
            if (!TestEqual(TEXT("miss uses wire"),Rig.Fake->NumCalls(),2)) return false;
            if (Mode==2)
            {
                TestEqual(TEXT("live B credentials"),Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("Authorization")),FString(TEXT("Bearer account-B-token")));
                TestEqual(TEXT("account shift skips storing old operation"),State->GetMemoryCache().Num(),0);
            }
        }
        else { TestEqual(TEXT("closed read no wire"),Rig.Fake->NumCalls(),1); TestEqual(TEXT("no resurrection"),State->GetMemoryCache().Num(),0); }
        TestFalse(TEXT("gate did not time out"),Barrier->TimedOut.load());
        TestTrue(TEXT("all work drained"),Rig.Drain() && Disk.Drain());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheReadContentionTest,"VaRest.Http.Cache.OptionalReadContention",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheReadContentionTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::CacheTestPrivate;
    FScopedPersistenceTest Disk; FScopedCacheControls Controls; FTestRig Rig; FCleanup Cleanup(Rig);
    Rig.Client.WithStorageDirectories(Disk.Root,Disk.Root).WithCache(FVaRestCachePolicy::MemoryAndDisk());
    const auto State=FClientAccess::State(Rig.Client); auto Store=State->GetDiskCacheFor(*State->GetConfig(),64*1024*1024);
    if (!TestTrue(TEXT("healthy default store initialized"),Disk.Drain())) return false;
    TestFalse(TEXT("not quarantined"),Store->IsQuarantined());
    FScopedPersistenceHooks Hooks(Directory(Rig)); auto Barrier=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->BeforeRead=Barrier;
    ON_SCOPE_EXIT { Barrier->Released=true; Disk.Drain(); Rig.Client.CancelAll(); Rig.Drain(); };
    Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
    {
        auto R=Response(200,Request.Url.EndsWith(TEXT("/second")) ? TEXT("second-own-wire") : TEXT("first-own-wire"));
        R.Headers.Set(TEXT("Set-Cookie"),TEXT("do-not-store")); return R;
    });
    auto A=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/first")),A).Send();
    if (!TestTrue(TEXT("first real cold read held"),PumpUntil([Barrier] { return Barrier->Entered.load(); }))) return false;
    const auto Launched=PersistenceCounters().Launched.load();
    TestEqual(TEXT("default read reserves 112 MiB"),PersistencePendingBytesForTest(),int64(112)*1024*1024);
    auto B=MakeOutcome<FString>(); auto H=Observe(Rig.Client.Get<FString>(TEXT("/second")),B).Send();
    TestFalse(TEXT("noninline fallback"),B->IsDone());
    TestTrue(TEXT("runtime still accepting"),PersistenceAccepting());
    if (!TestTrue(TEXT("second wire completes while first blocked"),Rig.Wait(B))) return false;
    TestEqual(TEXT("second read actually refused: no new executor job"),PersistenceCounters().Launched.load(),Launched);
    TestEqual(TEXT("refusal did not add reserved bytes"),PersistencePendingBytesForTest(),int64(112)*1024*1024);
    if (!TestTrue(TEXT("own second bytes present"),B->Value.IsSet())) return false;
    TestEqual(TEXT("own wire bytes"),B->Value.GetValue(),FString(TEXT("second-own-wire")));
    TestFalse(TEXT("not stale cache"),B->Response.bFromCache);
    TestTrue(TEXT("second handle done without wait"),!H.IsActive());
    TestFalse(TEXT("first still blocked"),A->IsDone());
    Barrier->Released=true;
    if (!TestTrue(TEXT("release first disk"),Disk.Drain()) || !TestTrue(TEXT("first wire"),Rig.Wait(A))) return false;
    TestEqual(TEXT("two physical responses"),Rig.Fake->NumCalls(),2);
    TestTrue(TEXT("drained"),Disk.Drain() && Rig.Drain()); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheAdmissionTest,"VaRest.Http.Cache.AdmissionBoundaries",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheAdmissionTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::CacheTestPrivate;
    FScopedPersistenceTest Disk; FScopedCacheControls Controls;
    {
        FScopedPersistenceRuntime Runtime; FTestRig Rig; FCleanup Cleanup(Rig);
        Rig.Client.WithStorageDirectories(Disk.Root,Disk.Root).WithCache(FVaRestCachePolicy::MemoryAndDisk());
        auto Capture=MakeShared<FCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
        TArray<uint64> Tickets; ON_SCOPE_EXIT { for (uint64 Ticket : Tickets) ReleasePersistenceCompletion(Ticket); };
        for (int32 Index=0;Index<8192;++Index)
        { const uint64 Ticket=AcquirePersistenceCompletion(); if (!TestTrue(TEXT("permit acquired"),Ticket!=0)) return false; Tickets.Add(Ticket); }
        const auto Launched=PersistenceCounters().Launched.load();
        const int32 Registered=CacheRegistryForTest();
        auto A=MakeOutcome<void>(); Observe(Rig.Client.Get<void>(TEXT("/permit")),A).Send();
        TestFalse(TEXT("permit refusal noninline"),A->IsDone());
        TestEqual(TEXT("no Open before permit"),CacheRegistryForTest(),Registered);
        TestEqual(TEXT("no worker launch before permit"),PersistenceCounters().Launched.load(),Launched);
        auto Ex=Capture->Seen.Pin(); if (!TestTrue(TEXT("refused exchange exists"),Ex.IsValid())) return false;
        TestFalse(TEXT("no interrupt"),bool(Internals(*Ex).Interrupt)); TestFalse(TEXT("no accepted persistence ownership"),Internals(*Ex).bPersistenceOwned);
        for (uint64 Ticket : Tickets) ReleasePersistenceCompletion(Ticket); Tickets.Reset();
        if (!TestTrue(TEXT("refusal delivered"),Rig.Wait(A))) return false;
        TestEqual(TEXT("permit Config"),A->Error.Kind,EVaRestErrorKind::Config); TestFalse(TEXT("nonretryable"),A->Error.bRetryable);
        TestEqual(TEXT("no refused wire"),Rig.Fake->NumCalls(),0);
        Rig.Fake->Enqueue(Response()); auto B=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/healthy")),B).Send();
        TestFalse(TEXT("healthy noninline"),B->IsDone());
        if (!TestTrue(TEXT("healthy disk handback"),Disk.Drain()) || !TestTrue(TEXT("healthy response"),Rig.Wait(B)) || !TestTrue(TEXT("healthy persistence"),Disk.Drain())) return false;
        TestEqual(TEXT("healthy wire"),Rig.Fake->NumCalls(),1);
        Runtime.Stop();
        auto C=MakeOutcome<void>(); Observe(Rig.Client.Get<void>(TEXT("/stopped")),C).Send(); TestFalse(TEXT("stopped noninline"),C->IsDone());
        if (!TestTrue(TEXT("closed runtime result"),Rig.Wait(C))) return false;
        TestEqual(TEXT("closed runtime Config"),C->Error.Kind,EVaRestErrorKind::Config); TestFalse(TEXT("closed nonretryable"),C->Error.bRetryable);
        TestEqual(TEXT("closed no wire"),Rig.Fake->NumCalls(),1);
        TestTrue(TEXT("closed drained"),Rig.Drain());
    }
    return TestTrue(TEXT("outer runtime restored"),Disk.Drain());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheQuarantineTest,"VaRest.Http.Cache.QuarantineIsMiss",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheQuarantineTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::CacheTestPrivate;
    FScopedPersistenceTest Disk; FScopedCacheControls Controls; FTestRig Rig; FCleanup Cleanup(Rig);
    Rig.Client.WithStorageDirectories(Disk.Root,Disk.Root).WithCache(FVaRestCachePolicy::MemoryAndDisk());
    FScopedPersistenceHooks Hooks(Directory(Rig)); Hooks.Hooks->bFailCacheControl=true;
    const auto State=FClientAccess::State(Rig.Client); auto Store=State->GetDiskCacheFor(*State->GetConfig(),64*1024*1024);
    if (!TestTrue(TEXT("failed maintenance drains"),Disk.Drain())) return false;
    TestTrue(TEXT("actual quarantine"),Store->IsQuarantined());
    Rig.Fake->Enqueue(Response(200,TEXT("safe-wire")));
    auto A=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/quarantine")),A).Send(); TestFalse(TEXT("noninline"),A->IsDone());
    if (!TestTrue(TEXT("handback"),Disk.Drain()) || !TestTrue(TEXT("wire response"),Rig.Wait(A))) return false;
    TestEqual(TEXT("quarantine does not fail live request"),A->SuccessCalls.load(),1);
    TestFalse(TEXT("no unsafe reused bytes"),A->Response.bFromCache);
    TestEqual(TEXT("one wire"),Rig.Fake->NumCalls(),1);
    return TestTrue(TEXT("drain"),Disk.Drain() && Rig.Drain());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheConditionalIdentityTest,"VaRest.Http.Cache.ConditionalIdentity",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheConditionalIdentityTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::CacheTestPrivate;
    for (bool Return304 : {false,true})
    {
        FTestRig Rig; FCleanup Cleanup(Rig); Rig.Client.WithCache(FVaRestCachePolicy::Memory()).WithAuth(FVaRestAuth::Bearer()).WithMaxConcurrent(1);
        Rig.Client.SetBearerToken(TEXT("account-A")); Rig.Fake->Enqueue(Response(200,TEXT("A-body"),TEXT("max-age=0")));
        auto A=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/conditional")),A).Send(); if (!TestTrue(TEXT("seed"),Rig.Wait(A))) return false;
        Rig.Fake->Enqueue(FVaRestFakeResponse::Manual()); auto Hold=MakeOutcome<void>(); Observe(Rig.Client.Get<void>(TEXT("/hold")).NoCache(),Hold).Send();
        if (!TestEqual(TEXT("slot occupied"),Rig.Fake->NumCalls(),2)) return false;
        auto Capture=MakeShared<FCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
        Rig.Fake->Enqueue(Response(Return304 ? 304 : 200,TEXT("B-wire")));
        auto B=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/conditional")),B).Send();
        auto Ex=Capture->Seen.Pin(); if (!TestTrue(TEXT("conditional exchange retained"),Ex.IsValid())) return false;
        TestTrue(TEXT("actual limiter queue"),Internals(*Ex).bInLimiterQueue); TestTrue(TEXT("cache validator injected"),Internals(*Ex).bCacheConditional);
        Rig.Client.SetBearerToken(TEXT("account-B")); Rig.Fake->Complete(1,Response());
        if (!TestTrue(TEXT("B delivered"),Rig.Wait(B)) || !TestEqual(TEXT("third physical call"),Rig.Fake->NumCalls(),3)) return false;
        const auto& Headers=Rig.Fake->GetCall(2).Request.Headers;
        TestFalse(TEXT("no A ETag to B"),Headers.Contains(TEXT("If-None-Match")));
        TestFalse(TEXT("no stale validator in retry template"),Internals(*Ex).RequestHeaders.Contains(TEXT("If-None-Match")));
        TestFalse(TEXT("no stale validator in attempt"),Internals(*Ex).AttemptHeaders.Contains(TEXT("If-None-Match")));
        TestEqual(TEXT("B token"),Headers.Get(TEXT("Authorization")),FString(TEXT("Bearer account-B")));
        TestFalse(TEXT("never merges another account"),B->Response.bFromCache);
        if (Return304) TestEqual(TEXT("foreign 304 remains HTTP"),B->Error.Kind,EVaRestErrorKind::Http);
        Rig.Client.SetBearerToken(TEXT("account-A"));
        TestEqual(TEXT("only initial entry remains"),FClientAccess::State(Rig.Client)->GetMemoryCache().Num(),1);
        TestTrue(TEXT("drained"),Rig.Drain());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheRetryRotationTest,"VaRest.Http.Cache.RetryRotationAndOfflineBoundary",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheRetryRotationTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::CacheTestPrivate;
    FTestRig Rig; FCleanup Cleanup(Rig); FScopedTraceRecorder Trace;
    Rig.Client.WithCache(FVaRestCachePolicy::Memory()).WithAuth(FVaRestAuth::Bearer()); Rig.Client.SetBearerToken(TEXT("A-original"));
    Rig.Fake->Enqueue(Response(200,TEXT("A-body"),TEXT("max-age=0"))); auto A=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/retry")),A).Send(); if (!TestTrue(TEXT("seed"),Rig.Wait(A))) return false;
    auto Retry=FVaRestRetryPolicy::Exponential(1); Retry.Backoff.BaseDelay=1; Retry.Backoff.Jitter=EVaRestJitter::None;
    Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection)); Rig.Fake->Enqueue(Response(304,TEXT("")));
    auto B=MakeOutcome<FString>(); auto H=Observe(Rig.Client.Get<FString>(TEXT("/retry")).Retry(Retry),B).Send();
    if (!TestTrue(TEXT("retry actually scheduled"),PumpUntil([&Trace,H] { return Notes(*Trace.Recorder,H.GetId(),EVaRestTraceNoteKind::RetryScheduled)==1; }))) return false;
    TestFalse(TEXT("physical failure observed before retry"),Rig.Connectivity->IsOnline());
    auto Offline=MakeOutcome<void>(); Observe(Rig.Client.Get<void>(TEXT("/new-miss")),Offline).Send();
    if (!TestTrue(TEXT("separate ordinary request"),Rig.Wait(Offline))) return false;
    TestEqual(TEXT("new request Offline"),Offline->Error.Kind,EVaRestErrorKind::Offline); TestEqual(TEXT("no new wire while offline"),Rig.Fake->NumCalls(),2);
    Rig.Client.SetBearerToken(TEXT("A-rotated"),true); Rig.Clock->Advance(1);
    if (!TestTrue(TEXT("retry allowed despite discovering offline"),Rig.Wait(B)) || !TestEqual(TEXT("retry physical call"),Rig.Fake->NumCalls(),3)) return false;
    TestEqual(TEXT("same identity validator remains"),Rig.Fake->GetCall(2).Request.Headers.Get(TEXT("If-None-Match")),FString(TEXT("\"one\"")));
    TestEqual(TEXT("rotated credential applied"),Rig.Fake->GetCall(2).Request.Headers.Get(TEXT("Authorization")),FString(TEXT("Bearer A-rotated")));
    TestTrue(TEXT("304 refresh keeps cached identity"),B->Response.bFromCache);
    TestTrue(TEXT("real 304 restores reachability"),Rig.Connectivity->IsOnline());
    return TestTrue(TEXT("drain"),Rig.Drain());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheStorageConfigTest,"VaRest.Http.Cache.StorageAndPolicyValidation",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheStorageConfigTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::CacheTestPrivate;
    FScopedPersistenceTest Disk; FScopedCacheControls Controls; FTestRig Rig; FCleanup Cleanup(Rig);
    Rig.Client.WithStorageDirectories(TEXT(""),TEXT(""));
    const auto State=FClientAccess::State(Rig.Client);
    if (!TestTrue(TEXT("invalid roots recorded"),!State->GetConfig()->StorageConfigError.IsEmpty())) return false;
    const auto Launched=PersistenceCounters().Launched.load(); const auto Registered=CacheRegistryForTest();
    auto A=MakeOutcome<void>(); Observe(Rig.Client.Get<void>(TEXT("/invalid")).Cache(FVaRestCachePolicy::MemoryAndDisk()),A).Send();
    TestFalse(TEXT("invalid noninline"),A->IsDone()); Rig.Client.ClearCache();
    if (!TestTrue(TEXT("invalid resolved"),Rig.Wait(A))) return false;
    TestEqual(TEXT("invalid disk Config"),A->Error.Kind,EVaRestErrorKind::Config);
    TestEqual(TEXT("invalid opens no store"),CacheRegistryForTest(),Registered); TestEqual(TEXT("invalid no IO"),PersistenceCounters().Launched.load(),Launched);
    for (int32 Mode=0;Mode<3;++Mode)
    {
        auto B=MakeOutcome<void>(); auto Request=Rig.Client.Get<void>(TEXT("/control"));
        if (Mode==1) Request=MoveTemp(Request).Cache(FVaRestCachePolicy::Memory());
        if (Mode==2) { Rig.Client.WithCache(FVaRestCachePolicy::MemoryAndDisk()); Request=MoveTemp(Request).NoCache(); }
        Observe(MoveTemp(Request),B).Send(); if (!TestTrue(TEXT("unused storage control"),Rig.Wait(B))) return false;
        TestEqual(TEXT("unused root does not fail HTTP"),B->SuccessCalls.load(),1);
    }
    for (int32 Mode=0;Mode<5;++Mode)
    {
        auto Policy=FVaRestCachePolicy::Memory();
        if (Mode==0) Policy.Mode=static_cast<EVaRestCacheMode>(255);
        if (Mode==1) Policy.MemoryBytes=-1;
        if (Mode==2) Policy.DiskBytes=-1;
        if (Mode==3) Policy.DefaultTtlSeconds=-1;
        if (Mode==4) Policy.DefaultTtlSeconds=std::numeric_limits<double>::infinity();
        Rig.Client.WithCache(Policy); auto B=MakeOutcome<void>(); Observe(Rig.Client.Get<void>(TEXT("/invalid-policy")),B).Send();
        TestFalse(TEXT("policy noninline"),B->IsDone()); if (!TestTrue(TEXT("invalid policy result"),Rig.Wait(B))) return false;
        TestEqual(TEXT("invalid policy Config"),B->Error.Kind,EVaRestErrorKind::Config);
        auto C=MakeOutcome<void>(); Observe(Rig.Client.Get<void>(TEXT("/override")).NoCache(),C).Send(); if (!TestTrue(TEXT("NoCache replaces invalid client policy"),Rig.Wait(C))) return false;
        TestEqual(TEXT("override valid"),C->SuccessCalls.load(),1);
    }
    Rig.Client.WithStorageDirectories(Disk.Root,Disk.Root).WithCache(FVaRestCachePolicy::MemoryAndDisk());
    auto D=MakeOutcome<void>(); Observe(Rig.Client.Get<void>(TEXT("/corrected")),D).Send();
    if (!TestTrue(TEXT("corrected disk lookup"),Disk.Drain()) || !TestTrue(TEXT("corrected request"),Rig.Wait(D))) return false;
    TestEqual(TEXT("corrected success"),D->SuccessCalls.load(),1);
    return TestTrue(TEXT("drain"),Disk.Drain() && Rig.Drain());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheClearNamespaceTest,"VaRest.Http.Cache.ClearCurrentUnopenedNamespace",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheClearNamespaceTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::CacheTestPrivate;
    FScopedPersistenceTest Disk; FScopedCacheControls Controls;
    for (bool OldPointer : {false,true})
    {
        FTestRig Seed; FCleanup SeedCleanup(Seed); FTestRig Reader; FCleanup ReaderCleanup(Reader);
        const FString Root=FPaths::Combine(Disk.Root,OldPointer ? TEXT("changed") : TEXT("unopened"));
        auto Policy=FVaRestCachePolicy::MemoryAndDisk(); Policy.DefaultTtlSeconds=60;
        Seed.Client.WithStorageDirectories(Root,Disk.Root).WithCache(Policy);
        Seed.Fake->Enqueue(Response(200,TEXT("old-disk"))); auto A=MakeOutcome<FString>(); Observe(Seed.Client.Get<FString>(TEXT("/current")),A).Send();
        if (!TestTrue(TEXT("seed lookup"),Disk.Drain()) || !TestTrue(TEXT("seed outcome"),Seed.Wait(A)) || !TestTrue(TEXT("seed persisted"),Disk.Drain())) return false;
        if (OldPointer)
        {
            Reader.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("previous")),Disk.Root).WithCache(Policy);
            auto State=FClientAccess::State(Reader.Client); State->GetDiskCacheFor(*State->GetConfig(),Policy.DiskBytes);
            if (!TestTrue(TEXT("old store ready"),Disk.Drain())) return false;
        }
        Reader.Client.WithStorageDirectories(Root,Disk.Root).WithCache(Policy); Reader.Client.ClearCache();
        if (!TestTrue(TEXT("clear persisted"),Disk.Drain())) return false;
        Reader.Fake->Enqueue(Response(200,TEXT("new-wire"))); auto B=MakeOutcome<FString>(); Observe(Reader.Client.Get<FString>(TEXT("/current")),B).Send();
        if (!TestTrue(TEXT("read cleared namespace"),Disk.Drain()) || !TestTrue(TEXT("current result"),Reader.Wait(B))) return false;
        TestFalse(TEXT("current unopened namespace was cleared"),B->Response.bFromCache); TestEqual(TEXT("wire required"),Reader.Fake->NumCalls(),1);
        TestTrue(TEXT("drain"),Disk.Drain() && Seed.Drain() && Reader.Drain());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheDeadlineStoreTest,"VaRest.Http.Cache.LiveDeadlineAndStoreEpoch",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheDeadlineStoreTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::CacheTestPrivate;
    for (int32 Mode=0;Mode<5;++Mode)
    {
        FTestRig Rig; FCleanup Cleanup(Rig); FScopedTraceRecorder Trace;
        const auto Clock=MakeShared<FHeldClock,ESPMode::ThreadSafe>(); auto Capture=MakeShared<FCapture,ESPMode::ThreadSafe>();
        Rig.Client.WithClock(Clock).WithCache(FVaRestCachePolicy::Memory()).Use(Capture,EVaRestStageSlot::Outer);
        Rig.Fake->Enqueue(Response()); auto A=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/deadline")),A).Send(); if (!TestTrue(TEXT("seed"),Rig.Wait(A))) return false;
        const auto State=FClientAccess::State(Rig.Client);
        if (Mode==0 || Mode==1) Capture->bHold=true;
        else { State->GetMemoryCache().Clear(); Rig.Fake->Enqueue(FVaRestFakeResponse::Manual()); }
        auto B=MakeOutcome<FString>(); auto Request=Rig.Client.Get<FString>(TEXT("/deadline"));
        UObject* Owner=nullptr; ON_SCOPE_EXIT { Capture->Release(); };
        if (Mode==1) { Owner=NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient); Request=MoveTemp(Request).Owner(Owner); }
        if (Mode==0 || Mode==2) Request=MoveTemp(Request).Retry(FVaRestRetryPolicy::Disabled().WithDeadline(1));
        auto H=Observe(MoveTemp(Request),B).Send();
        if (Mode==0 || Mode==2) Clock->Seconds+=1;
        if (Mode==1) Owner->MarkAsGarbage();
        if (Mode==3) Rig.Client.ClearCache();
        if (Mode==4) H.Cancel();
        if (Mode<2) Capture->Release();
        else
        {
            if (!TestEqual(TEXT("actual held wire"),Rig.Fake->NumCalls(),2)) return false;
            Rig.Fake->Complete(1,Response(200,TEXT("late-body")));
        }
        if (!TestTrue(TEXT("request terminates"),PumpUntil([H] { return !H.IsActive(); }))) return false;
        if (Mode!=1 && !TestTrue(TEXT("typed completion"),Rig.Wait(B))) return false;
        if (Mode==0 || Mode==2) TestEqual(TEXT("live deadline wins"),B->Error.Kind,EVaRestErrorKind::Timeout);
        if (Mode==1) TestEqual(TEXT("gone owner silent"),B->CompleteCalls.load(),0);
        TestEqual(TEXT("late memory store forbidden"),Notes(*Trace.Recorder,H.GetId(),EVaRestTraceNoteKind::CacheStored),0);
        if (Mode>=2) TestEqual(TEXT("no resurrection after deadline clear or cancel"),State->GetMemoryCache().Num(),0);
        TestEqual(TEXT("no late revalidation"),Notes(*Trace.Recorder,H.GetId(),EVaRestTraceNoteKind::CacheRevalidated),0);
        TestFalse(TEXT("no cache provenance after closure"),B->Response.bFromCache);
        TestTrue(TEXT("drain"),Rig.Drain());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheStoreNotesTest,"VaRest.Http.Cache.StoredNotesAndDiskGuard",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheStoreNotesTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::CacheTestPrivate;
    for (int32 Mode=0;Mode<5;++Mode)
    {
        FScopedPersistenceTest Disk; FScopedCacheControls Controls; FTestRig Rig; FCleanup Cleanup(Rig); FScopedTraceRecorder Trace;
        auto Policy=FVaRestCachePolicy::MemoryAndDisk(); Policy.DefaultTtlSeconds=60; if (Mode==1) Policy.MemoryBytes=0;
        Rig.Client.WithStorageDirectories(Disk.Root,Disk.Root).WithCache(Policy);
        if (Mode==2) Rig.Client.SetSecretValue(TEXT("snapshot"),TEXT("secret-response"));
        Rig.Fake->Enqueue(FVaRestFakeResponse::Manual()); auto A=MakeOutcome<FString>(); auto H=Observe(Rig.Client.Get<FString>(TEXT("/notes")),A).Send();
        if (Mode==4) Rig.Client.SetSecretValue(TEXT("applied"),TEXT("secret-response"));
        if (!TestTrue(TEXT("lookup hands off to held wire"),PumpUntil([&Rig] { return Rig.Fake->NumCalls()==1; },5,true)) || !TestEqual(TEXT("wire held"),Rig.Fake->NumCalls(),1)) return false;
        if (Mode==2 || Mode==4) Rig.Client.ClearSecretValues();
        if (Mode==3) Rig.Client.SetSecretValue(TEXT("late"),TEXT("secret-response"));
        Rig.Fake->Complete(0,Response(200,TEXT("secret-response")));
        if (!TestTrue(TEXT("live result"),Rig.Wait(A))) return false;
        const auto AtCompletion=Trace.Recorder->For(H.GetId());
        const int32 Stored=Notes(*Trace.Recorder,H.GetId(),EVaRestTraceNoteKind::CacheStored);
        TestEqual(TEXT("CacheStored means synchronous memory retention"),Stored,Mode==1 ? 0 : 1);
        bool CompletedSeen=false;
        for (const auto& Event : AtCompletion)
        {
            if (Event.Kind==EVaRestTraceKind::Completed) CompletedSeen=true;
            if (Event.Kind==EVaRestTraceKind::Note && Event.Payload.Get<FVaRestTraceNote>().Kind==EVaRestTraceNoteKind::CacheStored)
            {
                TestFalse(TEXT("stored note precedes Completed"),CompletedSeen);
                TestEqual(TEXT("note encoded bytes"),Event.Payload.Get<FVaRestTraceNote>().Bytes,int64(15));
            }
        }
        TestTrue(TEXT("completion observed"),CompletedSeen);
        if (!TestTrue(TEXT("disk put handback"),Disk.Drain())) return false;
        TestEqual(TEXT("best effort disk has no late notes"),Notes(*Trace.Recorder,H.GetId(),EVaRestTraceNoteKind::CacheStored),Stored);
        FClientAccess::State(Rig.Client)->GetMemoryCache().Clear();
        Rig.Fake->Enqueue(Response(200,TEXT("safe-wire"))); auto B=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/notes")),B).Send();
        if (!TestTrue(TEXT("cold read handback"),Disk.Drain()) || !TestTrue(TEXT("cold result"),Rig.Wait(B))) return false;
        TestEqual(TEXT("disk-only safe control, snapshot/current secrets refused"),B->Response.bFromCache,Mode<2);
        TestTrue(TEXT("drain"),Disk.Drain() && Rig.Drain());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheInvalidationTest,"VaRest.Http.Cache.NoStoreInvalidationAndLastModified",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheInvalidationTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::CacheTestPrivate;
    for (bool NotModified : {false,true})
    {
        FScopedPersistenceTest Disk; FScopedCacheControls Controls; FTestRig Rig; FCleanup Cleanup(Rig);
        Rig.Client.WithCache(FVaRestCachePolicy::MemoryAndDisk()).WithStorageDirectories(Disk.Root,Disk.Root);
        auto First=Response(200,TEXT("original"),TEXT("max-age=0")); First.Headers.Remove(TEXT("ETag")); First.Headers.Set(TEXT("Last-Modified"),TEXT("Wed, 21 Oct 2015 07:28:00 GMT"));
        Rig.Fake->Enqueue(MoveTemp(First)); auto A=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/remove")),A).Send();
        if (!TestTrue(TEXT("seed lookup"),Disk.Drain()) || !TestTrue(TEXT("seed"),Rig.Wait(A)) || !TestTrue(TEXT("seed disk"),Disk.Drain())) return false;
        Rig.Fake->Enqueue(Response(NotModified ? 304 : 200,TEXT(""),TEXT("no-store")));
        auto B=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/remove")),B).Send();
        if (!TestTrue(TEXT("no-store response"),Rig.Wait(B)) || !TestEqual(TEXT("two calls"),Rig.Fake->NumCalls(),2)) return false;
        TestEqual(TEXT("Last-Modified validator"),Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("If-Modified-Since")),FString(TEXT("Wed, 21 Oct 2015 07:28:00 GMT")));
        TestEqual(TEXT("304 may satisfy current captured bytes"),B->Response.bFromCache,NotModified);
        if (!TestTrue(TEXT("invalidation durable"),Disk.Drain())) return false;
        TestEqual(TEXT("memory removed"),FClientAccess::State(Rig.Client)->GetMemoryCache().Num(),0);
        Rig.Fake->Enqueue(Response(200,TEXT("replacement"))); auto C=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/remove")),C).Send();
        if (!TestTrue(TEXT("cold miss"),Disk.Drain()) || !TestTrue(TEXT("new wire"),Rig.Wait(C))) return false;
        TestFalse(TEXT("disk also removed"),C->Response.bFromCache); TestEqual(TEXT("new call required"),Rig.Fake->NumCalls(),3);
        TestTrue(TEXT("drain"),Disk.Drain() && Rig.Drain());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheRetainedHeadersTest,"VaRest.Http.Cache.RetainedHeaders",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheRetainedHeadersTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::CacheTestPrivate;
    for (bool Head : {false,true})
    {
        FTestRig Rig; FCleanup Cleanup(Rig);
        Rig.Client.WithCache(FVaRestCachePolicy::Memory()); Rig.Client.SetSessionHeader(TEXT("X-Tenant-Session"),TEXT("private"));
        Rig.Client.WithRedactedKeys({TEXT("X-Custom-Private")});
        auto First=Response(200,Head ? TEXT("") : TEXT("encoded"),TEXT("max-age=60"));
        First.Headers.Set(TEXT("Content-Length"),TEXT("7123")); First.Headers.Set(TEXT("Connection"),TEXT("X-Hop"));
        First.Headers.Set(TEXT("X-Hop"),TEXT("hop")); First.Headers.Set(TEXT("Transfer-Encoding"),TEXT("chunked"));
        First.Headers.Set(TEXT("Authorization"),TEXT("response-auth")); First.Headers.Set(TEXT("X-Tenant-Session"),TEXT("response-session"));
        First.Headers.Set(TEXT("X-Custom-Private"),TEXT("response-custom"));
        Rig.Fake->Enqueue(MoveTemp(First)); auto A=MakeOutcome<void>();
        Observe(Head ? Rig.Client.Head<void>(TEXT("/headers")) : Rig.Client.Get<void>(TEXT("/headers")),A).Send();
        if (!TestTrue(TEXT("original response"),Rig.Wait(A))) return false;
        TestEqual(TEXT("wire headers unchanged"),A->Response.Headers.Get(TEXT("Content-Length")),FString(TEXT("7123")));
        TestTrue(TEXT("wire response auth remains caller-visible"),A->Response.Headers.Contains(TEXT("Authorization")));
        auto B=MakeOutcome<void>(); Observe(Head ? Rig.Client.Head<void>(TEXT("/headers")) : Rig.Client.Get<void>(TEXT("/headers")),B).Send();
        if (!TestTrue(TEXT("cached response"),Rig.Wait(B))) return false;
        TestTrue(TEXT("actual cache hit"),B->Response.bFromCache);
        TestEqual(TEXT("retained GET actual bytes; HEAD representation length"),B->Response.Headers.Get(TEXT("Content-Length")),FString(Head ? TEXT("7123") : TEXT("7")));
        for (const TCHAR* Name : {TEXT("Connection"),TEXT("X-Hop"),TEXT("Transfer-Encoding"),TEXT("Authorization"),TEXT("X-Tenant-Session"),TEXT("X-Custom-Private")})
            TestFalse(FString::Printf(TEXT("retained header excluded: %s"),Name),B->Response.Headers.Contains(Name));
        Rig.Clock->Advance(60);
        auto Refresh=Response(304,TEXT(""),TEXT("max-age=60")); Refresh.Headers.Set(TEXT("X-Tenant-Session"),TEXT("new-session")); Refresh.Headers.Set(TEXT("X-Custom-Private"),TEXT("new-custom"));
        Rig.Fake->Enqueue(MoveTemp(Refresh)); auto C=MakeOutcome<void>(); Observe(Head ? Rig.Client.Head<void>(TEXT("/headers")) : Rig.Client.Get<void>(TEXT("/headers")),C).Send();
        if (!TestTrue(TEXT("revalidation"),Rig.Wait(C))) return false;
        TestTrue(TEXT("304 representation"),C->Response.bFromCache);
        TestFalse(TEXT("304 cannot reintroduce session response header"),C->Response.Headers.Contains(TEXT("X-Tenant-Session")));
        TestFalse(TEXT("304 cannot reintroduce custom private response header"),C->Response.Headers.Contains(TEXT("X-Custom-Private")));
        TestTrue(TEXT("drain"),Rig.Drain());
    }
    FTestRig Rig; FCleanup Cleanup(Rig);
    Rig.Client.WithCache(FVaRestCachePolicy::Memory()).WithRedactedKeys({TEXT("ETag"),TEXT("Last-Modified")});
    auto First=Response(200,TEXT("bytes"),TEXT("max-age=0")); First.Headers.Set(TEXT("Last-Modified"),TEXT("Wed, 21 Oct 2015 07:28:00 GMT"));
    Rig.Fake->Enqueue(MoveTemp(First)); auto A=MakeOutcome<void>(); Observe(Rig.Client.Get<void>(TEXT("/protected-validator")),A).Send();
    if (!TestTrue(TEXT("validator seed"),Rig.Wait(A))) return false;
    Rig.Fake->Enqueue(Response()); auto B=MakeOutcome<void>(); Observe(Rig.Client.Get<void>(TEXT("/protected-validator")),B).Send();
    if (!TestTrue(TEXT("validator second"),Rig.Wait(B)) || !TestEqual(TEXT("validator wire calls"),Rig.Fake->NumCalls(),2)) return false;
    TestFalse(TEXT("protected ETag cannot survive metadata"),Rig.Fake->GetCall(1).Request.Headers.Contains(TEXT("If-None-Match")));
    TestFalse(TEXT("protected Last-Modified cannot survive metadata"),Rig.Fake->GetCall(1).Request.Headers.Contains(TEXT("If-Modified-Since")));
    TestTrue(TEXT("validator drain"),Rig.Drain());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheFallbackTest,"VaRest.Http.Cache.ProcessFallbackIsMiss",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheFallbackTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::CacheTestPrivate;
    FScopedPersistenceTest Disk; FScopedCacheControls Controls; FTestRig Rig; FCleanup Cleanup(Rig);
    auto Invalid=FDiskCache::Open(FPaths::Combine(Disk.Root,FString::ChrN(5000,TEXT('x'))),64*1024*1024);
    TestTrue(TEXT("actual bounded control fallback"),Invalid->IsQuarantined());
    Rig.Client.WithStorageDirectories(Disk.Root,Disk.Root).WithCache(FVaRestCachePolicy::MemoryAndDisk());
    const auto Launched=PersistenceCounters().Launched.load();
    Rig.Fake->Enqueue(Response(200,TEXT("fallback-wire"))); auto A=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/fallback")),A).Send();
    TestFalse(TEXT("fallback noninline"),A->IsDone()); if (!TestTrue(TEXT("fallback response"),Rig.Wait(A))) return false;
    TestEqual(TEXT("fallback successful wire"),A->SuccessCalls.load(),1); TestFalse(TEXT("no disk bytes reused"),A->Response.bFromCache);
    TestEqual(TEXT("no disk work or retries under fallback"),PersistenceCounters().Launched.load(),Launched);
    return TestTrue(TEXT("drain"),Disk.Drain() && Rig.Drain());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheForceValidationTest,"VaRest.Http.Cache.RequestValidationAndProbeBypass",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheForceValidationTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::CacheTestPrivate;
    for (const TCHAR* Control : {TEXT("no-cache"),TEXT("max-age=0")}) for (bool bSessionOverlay : {false,true})
    {
        FTestRig Rig; FCleanup Cleanup(Rig); auto Policy=FVaRestCachePolicy::Memory(); Policy.bServeStaleOnError=true;
        if (bSessionOverlay) Rig.Client.SetSessionHeader(TEXT("Cache-Control"),TEXT("max-age=60"));
        Rig.Client.WithCache(Policy); Rig.Fake->Enqueue(Response()); Rig.Fake->Enqueue(Response(304,TEXT("")));
        for (int32 Index=0;Index<2;++Index)
        {
            auto A=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/validation")).Header(TEXT("Cache-Control"),Control),A).Send();
            if (!TestTrue(TEXT("forced validation completes"),Rig.Wait(A))) return false;
        }
        TestEqual(TEXT("fresh bytes require wire validation"),Rig.Fake->NumCalls(),2);
        Rig.Connectivity->ReportOffline(); auto B=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/validation")).Header(TEXT("Cache-Control"),Control),B).Send();
        if (!TestTrue(TEXT("forced offline validation"),Rig.Wait(B))) return false;
        TestEqual(TEXT("fresh/stale forbidden by request directive offline"),B->Error.Kind,EVaRestErrorKind::Offline);
        TestTrue(TEXT("drain"),Rig.Drain());
    }
    FTestRig Rig; FCleanup Cleanup(Rig); Rig.Client.WithCache(FVaRestCachePolicy::Memory());
    Rig.Fake->Enqueue(Response(200,TEXT(""))); auto Seed=MakeOutcome<void>(); Observe(Rig.Client.Head<void>(TEXT("/health")).NoAuth(),Seed).Send();
    if (!TestTrue(TEXT("health representation seeded"),Rig.Wait(Seed))) return false;
    const auto State=FClientAccess::State(Rig.Client); const int64 Lookups=State->GetCounters().CacheLookups.load();
    Rig.Client.WithHealthUrl(TEXT("https://api.test/health")); Rig.Connectivity->ReportOffline();
    Rig.Fake->Enqueue(Response(200,TEXT(""))); Rig.Clock->Advance(10);
    if (!TestTrue(TEXT("health probe drains"),Rig.Drain())) return false;
    TestEqual(TEXT("probe bypasses fresh HEAD cache and offline refusal"),Rig.Fake->NumCalls(),2);
    TestEqual(TEXT("probe no cache lookup"),State->GetCounters().CacheLookups.load(),Lookups);
    TestTrue(TEXT("physical probe observes online"),Rig.Connectivity->IsOnline());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheSessionHeadersTest,"VaRest.Http.Cache.SessionHeaders",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheSessionHeadersTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::CacheTestPrivate;
    const TPair<FString,FString> Headers[]={
        {TEXT("Range"),TEXT("bytes=0-1")}, {TEXT("If-None-Match"),TEXT("caller")},
        {TEXT("If-Modified-Since"),TEXT("caller")}, {TEXT("If-Range"),TEXT("caller")},
        {TEXT("Cache-Control"),TEXT("no-store")}, {TEXT("Cache-Control"),TEXT("max-age=60")}};
    for (int32 Mode=0;Mode<6;++Mode) for (bool bNoAuth : {false,true})
    {
        FTestRig Rig; FCleanup Cleanup(Rig); Rig.Client.WithCache(FVaRestCachePolicy::Memory());
        const auto Capture=MakeShared<FCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
        Rig.Client.SetSessionHeader(Headers[Mode].Key,Headers[Mode].Value);
        // The last row preserves caller no-store even when the applied session value overwrites it.
        const bool bEligible=bNoAuth && Mode!=5;
        for (int32 Index=0;Index<2;++Index)
        {
            if (!bEligible || Index==0) Rig.Fake->Enqueue(Response());
            auto A=MakeOutcome<FString>(); auto Request=Rig.Client.Get<FString>(TEXT("/session"));
            if (bNoAuth) Request=MoveTemp(Request).NoAuth();
            if (Mode==5) Request=MoveTemp(Request).Header(TEXT("Cache-Control"),TEXT("no-store"));
            Observe(MoveTemp(Request),A).Send();
            if (!TestTrue(TEXT("session row completes"),Rig.Wait(A))) return false;
            TestEqual(TEXT("Resolve pending reflects known eligibility"),Capture->bPendingAtEntry,bEligible);
            TestEqual(TEXT("session row provenance"),A->Response.bFromCache,bEligible && Index==1);
        }
        const auto& Counters=FClientAccess::State(Rig.Client)->GetCounters();
        TestEqual(TEXT("known session bypass has no lookup"),Counters.CacheLookups.load(),int64(bEligible ? 2 : 0));
        TestEqual(TEXT("only NoAuth control hits"),Counters.CacheHits.load(),int64(bEligible ? 1 : 0));
        if (!TestEqual(TEXT("wire count"),Rig.Fake->NumCalls(),bEligible ? 1 : 2)) return false;
        TestEqual(TEXT("actual session overlay matches AuthApply"),Rig.Fake->GetCall(0).Request.Headers.Get(Headers[Mode].Key),
            bNoAuth ? (Mode==5 ? FString(TEXT("no-store")) : FString()) : Headers[Mode].Value);
        TestTrue(TEXT("drain"),Rig.Drain());
    }
    for (const TCHAR* Control : {TEXT("no-cache"),TEXT("max-age=0")}) for (bool bNoAuth : {false,true})
    {
        FTestRig Rig; FCleanup Cleanup(Rig); Rig.Client.WithCache(FVaRestCachePolicy::Memory());
        // Keep exactly the same session header/key on seed and repeat: a key change cannot mask a fresh hit.
        Rig.Client.SetSessionHeader(TEXT("Cache-Control"),Control); Rig.Fake->Enqueue(Response());
        if (!bNoAuth) Rig.Fake->Enqueue(Response(304,TEXT("")));
        for (int32 Index=0;Index<2;++Index)
        {
            auto A=MakeOutcome<FString>(); auto Request=Rig.Client.Get<FString>(TEXT("/session-validation"));
            if (bNoAuth) Request=MoveTemp(Request).NoAuth();
            Observe(MoveTemp(Request),A).Send(); if (!TestTrue(TEXT("session validation completes"),Rig.Wait(A))) return false;
            TestEqual(TEXT("repeat returns representation"),A->Response.bFromCache,Index==1);
        }
        if (!TestEqual(TEXT("session validation forces wire; NoAuth hits fresh"),Rig.Fake->NumCalls(),bNoAuth ? 1 : 2)) return false;
        if (!bNoAuth) TestEqual(TEXT("session validation carries validator"),Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("If-None-Match")),FString(TEXT("\"one\"")));
        TestTrue(TEXT("drain"),Rig.Drain());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheInherited304PolicyTest,"VaRest.Http.Cache.Inherited304Policy",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheInherited304PolicyTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::CacheTestPrivate;
    for (int32 Mode=0;Mode<9;++Mode)
    {
        FTestRig Rig; FCleanup Cleanup(Rig); auto Policy=FVaRestCachePolicy::Memory();
        Policy.DefaultTtlSeconds=60; Policy.bServeStaleOnError=true; Rig.Client.WithCache(Policy);
        if (Mode<6) Rig.Client.WithRedactedKeys({Mode==3 ? TEXT("Age") : TEXT("Cache-Control")});
        auto Capture=MakeShared<FCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
        auto Seed=Response(200,TEXT("policy-bytes"),(Mode==5 || Mode==6) ? TEXT("max-age=0") : Mode==0 ? TEXT("no-cache") : Mode==1 ? TEXT("max-age=0,must-revalidate") : Mode==2 ? TEXT("max-age=1") : TEXT("max-age=60"));
        if (Mode==3) Seed.Headers.Set(TEXT("Age"),TEXT("120"));
        Rig.Fake->Enqueue(MoveTemp(Seed)); auto A=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/inherited-policy")),A).Send();
        auto FirstEx=Capture->Seen.Pin(); if (!TestTrue(TEXT("seed exchange"),FirstEx.IsValid())) return false;
        if (!TestTrue(TEXT("policy seed"),Rig.Wait(A))) return false;
        const FString Key=Internals(*FirstEx).LookupKey;
        if (Mode==2) Rig.Clock->Advance(1);
        if (Mode==4 || Mode>=7) Rig.Clock->Advance(60);
        auto Modified=Response(304,TEXT("")); Modified.Headers.Remove(TEXT("Cache-Control"));
        if (Mode>=7)
        {
            Modified.Headers.Set(TEXT("Cache-Control"),TEXT("max-age=60"));
            Modified.Headers.Set(TEXT("Date"),Mode==7 ? FString(TEXT("invalid")) : FDateTime::UtcNow().ToHttpDate());
        }
        if (Mode==5) Modified.Headers.Set(TEXT("Cache-Control"),TEXT("max-age=60"));
        if (Mode==6) Modified.Headers.Set(TEXT("Cache-Control"),TEXT("public"));
        if (Mode==4)
        {
            // The filtered max-age equals the client default; numerical equality cannot prove provenance.
            const auto Date=FDateTime::UtcNow(); Modified.Headers.Set(TEXT("Date"),Date.ToHttpDate());
            Modified.Headers.Set(TEXT("Expires"),(Date+FTimespan::FromHours(1)).ToHttpDate());
        }
        Rig.Fake->Enqueue(MoveTemp(Modified)); auto B=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/inherited-policy")),B).Send();
        auto SecondEx=Capture->Seen.Pin(); if (!TestTrue(TEXT("304 exchange"),SecondEx.IsValid())) return false;
        if (!TestTrue(TEXT("captured304 response"),Rig.Wait(B)) || !TestEqual(TEXT("conditional wire"),Rig.Fake->NumCalls(),2)) return false;
        TestTrue(TEXT("304 still satisfies current bytes"),B->Response.bFromCache);
        if (!TestTrue(TEXT("304 body"),B->Value.IsSet())) return false;
        TestEqual(TEXT("captured body unchanged"),B->Value.GetValue(),FString(TEXT("policy-bytes")));
        if (Mode<6) TestFalse(TEXT("protected policy header stays omitted"),B->Response.Headers.Contains(Mode==3 ? TEXT("Age") : TEXT("Cache-Control")));
        TestEqual(TEXT("unchanged key"),Internals(*SecondEx).LookupKey,Key);
        if (Mode==1) Rig.Connectivity->ReportOffline();
        if (Mode==2) Rig.Clock->Advance(1);
        Rig.Fake->Enqueue(Response(200,TEXT("next-own-wire")));
        auto C=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/inherited-policy")),C).Send();
        if (!TestTrue(TEXT("following request"),Rig.Wait(C))) return false;
        TestEqual(TEXT("only explicit valid fresh policy permits hit"),C->Response.bFromCache,Mode==8);
        if (Mode==1) TestEqual(TEXT("inherited must-revalidate forbids offline stale"),C->Error.Kind,EVaRestErrorKind::Offline);
        else
        {
            TestEqual(TEXT("inherited restriction or valid-date control wire count"),Rig.Fake->NumCalls(),Mode==8 ? 2 : 3);
            if (!TestTrue(TEXT("following wire body"),C->Value.IsSet())) return false;
            TestEqual(TEXT("following response owns new bytes"),C->Value.GetValue(),FString(Mode==8 ? TEXT("policy-bytes") : TEXT("next-own-wire")));
        }
        TestTrue(TEXT("drain"),Rig.Drain());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestCacheRaw304NoStoreTest,"VaRest.Http.Cache.Raw304NoStore",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestCacheRaw304NoStoreTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::CacheTestPrivate;
    FScopedPersistenceTest Disk; FScopedCacheControls Controls; FTestRig Rig; FCleanup Cleanup(Rig); FScopedTraceRecorder Trace;
    auto Policy=FVaRestCachePolicy::MemoryAndDisk(); Policy.DefaultTtlSeconds=60;
    Rig.Client.WithStorageDirectories(Disk.Root,Disk.Root).WithCache(Policy);
    auto Capture=MakeShared<FCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
    const auto State=FClientAccess::State(Rig.Client);
    Rig.Fake->Enqueue(Response(200,TEXT("durable-original"),TEXT("max-age=0")));
    auto A=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/raw304")),A).Send();
    auto SeedEx=Capture->Seen.Pin(); if (!TestTrue(TEXT("seed exchange"),SeedEx.IsValid())) return false;
    if (!TestTrue(TEXT("seed lookup"),Disk.Drain()) || !TestTrue(TEXT("seed response"),Rig.Wait(A)) || !TestTrue(TEXT("seed durable"),Disk.Drain())) return false;
    const FString Key=Internals(*SeedEx).LookupKey,Identity=Internals(*SeedEx).LookupIdentity;
    TestTrue(TEXT("seed memory retained"),State->GetMemoryCache().Find(Key).IsValid());
    State->GetMemoryCache().Clear();
    auto Modified=Response(304,TEXT(""),TEXT("no-store")); Modified.Headers.Set(TEXT("Connection"),TEXT("Cache-Control"));
    Rig.Fake->Enqueue(MoveTemp(Modified)); auto B=MakeOutcome<FString>(); auto BHandle=Observe(Rig.Client.Get<FString>(TEXT("/raw304")),B).Send();
    if (!TestTrue(TEXT("actual persisted seed read"),Disk.Drain()) || !TestTrue(TEXT("raw304 response"),Rig.Wait(B))) return false;
    TestTrue(TEXT("raw304 returns captured disk bytes"),B->Response.bFromCache);
    TestEqual(TEXT("current304 keeps revalidated note"),Notes(*Trace.Recorder,BHandle.GetId(),EVaRestTraceNoteKind::CacheRevalidated),1);
    if (!TestTrue(TEXT("captured bytes"),B->Value.IsSet())) return false;
    TestEqual(TEXT("captured original"),B->Value.GetValue(),FString(TEXT("durable-original")));
    TestFalse(TEXT("hop policy not returned"),B->Response.Headers.Contains(TEXT("Cache-Control")));
    if (!TestEqual(TEXT("conditional call count"),Rig.Fake->NumCalls(),2)) return false;
    TestEqual(TEXT("actual disk candidate supplies validator"),Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("If-None-Match")),FString(TEXT("\"one\"")));
    if (!TestTrue(TEXT("invalidation durable"),Disk.Drain())) return false;
    TestFalse(TEXT("raw304 no-store invalidates memory"),State->GetMemoryCache().Find(Key).IsValid());
    State->GetMemoryCache().Clear();
    FScopedPersistenceHooks Hooks(Directory(Rig)); auto Barrier=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->BeforeRead=Barrier;
    ON_SCOPE_EXIT { Barrier->Released=true; Disk.Drain(); Rig.Client.CancelAll(); Rig.Drain(); };
    Rig.Fake->Enqueue(Response(200,TEXT("cold-own-wire")));
    auto C=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/raw304")),C).Send();
    auto ColdEx=Capture->Seen.Pin(); if (!TestTrue(TEXT("cold exchange"),ColdEx.IsValid())) return false;
    if (!TestTrue(TEXT("cold disk lookup really held"),PumpUntil([Barrier] { return Barrier->Entered.load(); }))) return false;
    TestFalse(TEXT("cold lookup is not inline"),C->IsDone()); Barrier->Released=true;
    if (!TestTrue(TEXT("cold read handoff"),Disk.Drain()) || !TestTrue(TEXT("cold request result"),Rig.Wait(C))) return false;
    TestEqual(TEXT("same cold key"),Internals(*ColdEx).LookupKey,Key); TestEqual(TEXT("same cold identity"),Internals(*ColdEx).LookupIdentity,Identity);
    TestFalse(TEXT("raw304 no-store invalidates disk"),C->Response.bFromCache);
    if (!TestTrue(TEXT("cold own bytes"),C->Value.IsSet())) return false;
    TestEqual(TEXT("own wire after cold miss"),C->Value.GetValue(),FString(TEXT("cold-own-wire")));
    TestEqual(TEXT("third physical call"),Rig.Fake->NumCalls(),3);
    TestFalse(TEXT("owned gate did not expire"),Barrier->TimedOut.load());
    return TestTrue(TEXT("drain"),Disk.Drain() && Rig.Drain());
}

#endif
