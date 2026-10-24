// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "VaRestAuth.h"
#include "VaRestPersistence.h"
#include "VaRestOfflineInternal.h"
#include "VaRestRequestSpec.h"
#include "UObject/Package.h"
#include "VaRestHttpSettings.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestHttpStatsInternal.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestTraceCacheHitGolden, "VaRest.Http.Trace.CacheHitGolden",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestTraceCacheHitGolden::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    FTestRig Rig;
    FScopedTraceRecorder Trace;
    auto Policy=FVaRestCachePolicy::Memory(); Policy.DefaultTtlSeconds=60;
    Rig.Client.WithCache(Policy);
    Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("plain-response")));
    const auto First=MakeOutcome<FString>();
    const auto FirstHandle=Observe(Rig.Client.Get<FString>(TEXT("/cached")),First).Send();
    if (!TestTrue(TEXT("populate"),Rig.Wait(First))) return false;
    const auto Hit=MakeOutcome<FString>();
    const auto HitHandle=Observe(Rig.Client.Get<FString>(TEXT("/cached")),Hit).Send();
    if (!TestTrue(TEXT("cache hit"),Rig.Wait(Hit))) return false;
    TestEqual(TEXT("no fabricated attempt"),Trace.Recorder->KindsOf(HitHandle.GetId()),FString(TEXT("Started,Completed")));
    const auto Events=Trace.Recorder->For(HitHandle.GetId());
    if (!TestEqual(TEXT("two metadata events"),Events.Num(),2)) return false;
    if (!TestTrue(TEXT("payload kinds"),Events[0].Kind==EVaRestTraceKind::Started && Events[1].Kind==EVaRestTraceKind::Completed)) return false;
    const auto& Started=Events[0].Payload.Get<FVaRestTraceStarted>();
    TestEqual(TEXT("Started reports actual Memory policy"),Started.CachePolicy,FName(TEXT("Memory")));
    const auto& Ended=Events[1].Payload.Get<FVaRestTraceCompleted>();
    TestTrue(TEXT("initial pending, unresolved"),Started.bCacheLookupPending && Started.CacheLookup==EVaRestTraceCacheLookup::None);
    TestTrue(TEXT("final memory provenance"),Ended.bFromCache && Ended.CacheLookup==EVaRestTraceCacheLookup::HitMemory);
    TestEqual(TEXT("delivered cache status"),Ended.Response.Status,200);
    TestEqual(TEXT("delivered cached entity bytes, independent of zero wire bytes"),Ended.Response.BodyBytes,int64(14));
    TestTrue(TEXT("cached storage is memory"),Ended.Response.Storage==EVaRestTraceResponseStorage::Memory);
    TestEqual(TEXT("cached effective URL matches delivered result"),Ended.Response.EffectiveUrl,Hit->Response.EffectiveUrl);
    TestEqual(TEXT("default cache retains request URL"),Ended.Response.EffectiveUrl,FString(TEXT("https://api.test/cached")));
    TestEqual(TEXT("Started actual attempt zero"),Started.CurrentAttempt,0);
    TestTrue(TEXT("nonnegative logical duration"),Events[1].Time>=Events[0].Time && Hit->Response.Elapsed>=0);
    const auto& Counters=FClientAccess::State(Rig.Client)->GetCounters();
    TestEqual(TEXT("two logical lookups"),Counters.CacheLookups.load(),int64(2));
    TestEqual(TEXT("one logical hit"),Counters.CacheHits.load(),int64(1));
    TestEqual(TEXT("cache ratio"),FHttpStats::CacheRatio(Counters),0.5);
    TestEqual(TEXT("zero attempts"),Hit->Response.Attempts,0);
    TestEqual(TEXT("no wire bytes on hit"),Hit->Response.BytesDown,static_cast<int64>(0));
    TestEqual(TEXT("one hit callback"),Hit->CompleteCalls.load(),1);
    FString Why;
    TestTrue(TEXT("hit phase invariant"),PhasesHold(*Trace.Recorder,HitHandle.GetId(),Why));
    TestTrue(TEXT("wire phase control"),PhasesHold(*Trace.Recorder,FirstHandle.GetId(),Why));
    const auto SeedEvents=Trace.Recorder->For(FirstHandle.GetId());
    if (!TestTrue(TEXT("memory store note before completion"),SeedEvents.Num()==5 && SeedEvents[3].Kind==EVaRestTraceKind::Note)) return false;
    const auto& Stored=SeedEvents[3].Payload.Get<FVaRestTraceNote>();
    TestTrue(TEXT("actual retained memory note"),Stored.Kind==EVaRestTraceNoteKind::CacheStored);
    TestEqual(TEXT("memory note request"),Stored.RequestId,FirstHandle.GetId());
    TestEqual(TEXT("memory note actual attempt"),Stored.Attempt,1);
    TestEqual(TEXT("retained encoded bytes"),Stored.Bytes,int64(14));

    TestTrue(TEXT("drain"),Rig.Drain());
    return true;
}

namespace VaRest::Http::TraceCacheOfflinePrivate
{
    void Storage(Testing::FTestRig& Rig,const Testing::FScopedPersistenceTest& Disk)
    {
        Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")));
    }
    FVaRestFakeResponse CacheResponse(const TCHAR* Control=TEXT("max-age=60"))
    {
        auto Reply=FVaRestFakeResponse::Text(200,TEXT("original-body"));
        Reply.Headers.Set(TEXT("Cache-Control"),Control); Reply.Headers.Set(TEXT("ETag"),TEXT("\"golden\""));
        return Reply;
    }
    // Exercise an actually empty final cached URL without changing cache policy behavior.
    class FClearCachedOutcomeUrl final : public IVaRestStage
    {
    public:
        virtual FName GetStageName() const override { return TEXT("TraceClearCachedOutcomeUrl"); }
        virtual void Process(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done) override
        {
            Next.Proceed(Exchange,[Up=MoveTemp(Done)](FVaRestAttemptOutcome&& Outcome) mutable
            {
                if (Outcome.bFromCache) Outcome.EffectiveUrl.Reset();
                Up(MoveTemp(Outcome));
            });
        }
    };
    // Actual exchange capture also permits an Outer answer while the inner operation remains held.
    class FOuter final : public IVaRestStage, public TSharedFromThis<FOuter,ESPMode::ThreadSafe>
    {
    public:
        virtual FName GetStageName() const override { return TEXT("TraceGoldenOuter"); }
        virtual void Process(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done) override
        {
            Seen=Exchange;
            Held=MoveTemp(Done);
            Again=[Self=AsShared(),Exchange,Next]() mutable
            { Next.Proceed(Exchange,[Self](FVaRestAttemptOutcome&& Outcome){ Self->OnInner(MoveTemp(Outcome)); }); };
            Again();
        }
        void OnInner(FVaRestAttemptOutcome&& Outcome)
        {
            ++InnerCalls;
            if (bHold || (bRepeat && InnerCalls==1)) return;
            auto Callback=MoveTemp(Held); if (Callback) Callback(MoveTemp(Outcome));
        }
        void Answer(bool bCached=false)
        {
            auto Callback=MoveTemp(Held);
            if (Callback) { FVaRestAttemptOutcome Outcome; Outcome.Status=204; Outcome.bFromCache=bCached; Callback(MoveTemp(Outcome)); }
        }
        void Release() { Again.Reset(); Held.Reset(); Seen.Reset(); }
        TSharedPtr<FVaRestExchange,ESPMode::ThreadSafe> Seen;
        TUniqueFunction<void()> Again;
        FVaRestOutcomeCallback Held;
        int32 InnerCalls=0;
        bool bRepeat=false,bHold=false;
    };
    bool Sequence(FAutomationTestBase& Test,const Testing::FTraceRecorder& Trace,uint64 Id,const TCHAR* Expected,TArray<FVaRestTraceEvent>& Events)
    {
        if (!Test.TestEqual(TEXT("exact metadata order"),Trace.KindsOf(Id),FString(Expected))) return false;
        Events=Trace.For(Id);
        if (!Test.TestTrue(TEXT("Started and Completed bound record"),Events.Num()>=2
            && Events[0].Kind==EVaRestTraceKind::Started && Events.Last().Kind==EVaRestTraceKind::Completed)) return false;
        Test.TestTrue(TEXT("real request identity"),Id!=0 && Events[0].Payload.Get<FVaRestTraceStarted>().RequestId==Id
            && Events.Last().Payload.Get<FVaRestTraceCompleted>().RequestId==Id);
        Test.TestTrue(TEXT("nonnegative logical interval"),Events.Last().Time>=Events[0].Time);
        Test.TestTrue(TEXT("metadata on GT"),Trace.OnlyExcerptsOffGameThread());
        const auto& Completed=Events.Last().Payload.Get<FVaRestTraceCompleted>();
        Test.TestTrue(TEXT("explicit decode duration"),(Completed.DecodeStartAt<0 && Completed.DecodeEndAt<0)
            || (Completed.DecodeStartAt>=Events[0].Time && Completed.DecodeEndAt>=Completed.DecodeStartAt));
        TMap<int32,double> DispatchTimes;
        for (const auto& Event : Events)
        {
            if (Event.Kind==EVaRestTraceKind::AttemptDispatched)
            {
                const auto& Dispatch=Event.Payload.Get<FVaRestTraceAttemptDispatched>();
                Test.TestEqual(TEXT("dispatch real request"),Dispatch.RequestId,Id);
                Test.TestTrue(TEXT("numbered physical dispatch"),Dispatch.Attempt>0);
                Test.TestTrue(TEXT("explicit waits nonnegative"),Dispatch.AuthWait>=0 && Dispatch.LimiterWait>=0 && Dispatch.GapSeconds>=0);
                DispatchTimes.Add(Dispatch.Attempt,Event.Time);
            }
            else if (Event.Kind==EVaRestTraceKind::AttemptEnded)
            {
                const auto& End=Event.Payload.Get<FVaRestTraceAttemptEnded>();
                Test.TestEqual(TEXT("ended real request"),End.RequestId,Id);
                const auto* At=DispatchTimes.Find(End.Attempt);
                if (!Test.TestTrue(TEXT("paired dispatch"),At!=nullptr)) return false;
                Test.TestTrue(TEXT("explicit transport duration"),End.TransportDoneAt>=*At && Event.Time>=End.TransportDoneAt);
            }
        }

        FString Why; Test.TestTrue(TEXT("attempt phase arithmetic"),Testing::PhasesHold(Trace,Id,Why));
        return true;
    }
    bool Note(FAutomationTestBase& Test,const FVaRestTraceEvent& Event,uint64 Id,EVaRestTraceNoteKind Kind,int32 Attempt,const FString& QueueId=FString())
    {
        if (!Test.TestTrue(TEXT("note payload kind"),Event.Kind==EVaRestTraceKind::Note)) return false;
        const auto& Payload=Event.Payload.Get<FVaRestTraceNote>();
        Test.TestTrue(TEXT("exact note kind"),Payload.Kind==Kind);
        Test.TestEqual(TEXT("note real id"),Payload.RequestId,Id);
        Test.TestEqual(TEXT("actual note attempt"),Payload.Attempt,Attempt);
        Test.TestEqual(TEXT("note durable id"),Payload.QueueId,QueueId);
        return true;
    }
    bool Snapshot(FAutomationTestBase& Test,const Testing::FTraceRecorder& Recorder,uint64 Id,double StartedAt,FName Policy,bool bPending,EVaRestTraceCacheLookup Lookup)
    {
        const auto Snapshots=Recorder.GetSnapshots();
        if (!Test.TestEqual(TEXT("one attach snapshot"),Snapshots.Num(),1)) return false;
        const auto Rows=Snapshots[0].InFlight.FilterByPredicate([Id](const FVaRestTraceStarted& Row){ return Row.RequestId==Id; });
        if (!Test.TestEqual(TEXT("one coherent current request"),Rows.Num(),1)) return false;
        const auto& Row=Rows[0];
        Test.TestTrue(TEXT("partial record"),EnumHasAnyFlags(Row.Flags,EVaRestTraceRequestFlags::Partial));
        Test.TestEqual(TEXT("original start preserved"),Row.StartedAt,StartedAt);
        Test.TestEqual(TEXT("current policy"),Row.CachePolicy,Policy);
        Test.TestEqual(TEXT("current pending"),Row.bCacheLookupPending,bPending);
        Test.TestTrue(TEXT("current disposition"),Row.CacheLookup==Lookup);
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestTraceCacheDiskGolden,"VaRest.Http.Trace.CacheDiskGolden",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestTraceCacheDiskGolden::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::TraceCacheOfflinePrivate;
    FScopedPersistenceTest Disk; FTestRig Rig; Storage(Rig,Disk); FScopedTraceRecorder Trace;
    auto Policy=FVaRestCachePolicy::MemoryAndDisk(); Policy.MemoryBytes=0; Rig.Client.WithCache(Policy);
    Rig.Fake->Enqueue(CacheResponse()); const auto Seed=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/disk")),Seed).Send();
    if (!TestTrue(TEXT("seed and persistence"),Rig.Wait(Seed) && Rig.Drain())) return false;
    const auto Hit=MakeOutcome<FString>(); const auto Handle=Observe(Rig.Client.Get<FString>(TEXT("/disk")),Hit).Send();
    if (!TestTrue(TEXT("disk result"),Rig.Wait(Hit))) return false;
    TArray<FVaRestTraceEvent> Events; if (!Sequence(*this,*Trace.Recorder,Handle.GetId(),TEXT("Started,Completed"),Events)) return false;
    const auto& Start=Events[0].Payload.Get<FVaRestTraceStarted>(); const auto& End=Events.Last().Payload.Get<FVaRestTraceCompleted>();
    TestTrue(TEXT("initial not predicted disk hit"),Start.bCacheLookupPending && Start.CacheLookup==EVaRestTraceCacheLookup::None);
    TestEqual(TEXT("disk policy"),Start.CachePolicy,FName(TEXT("MemoryAndDisk")));
    TestTrue(TEXT("final disk provenance"),End.bFromCache && End.CacheLookup==EVaRestTraceCacheLookup::HitDisk);
    TestEqual(TEXT("attempts zero"),Hit->Response.Attempts,0); TestEqual(TEXT("start attempt zero"),Start.CurrentAttempt,0);
    TestEqual(TEXT("wire bytes zero"),Hit->Response.BytesDown,int64(0)); TestEqual(TEXT("no extra wire"),Rig.Fake->NumCalls(),1);
    TestEqual(TEXT("exactly one success"),Hit->SuccessCalls.load(),1); TestEqual(TEXT("exactly one completion"),Hit->CompleteCalls.load(),1);
    if (!TestTrue(TEXT("decoded value"),Hit->Value.IsSet())) return false;
    TestEqual(TEXT("fresh result decoded from retained bytes"),Hit->Value.GetValue(),FString(TEXT("original-body")));
    TestTrue(TEXT("drained"),Rig.Drain()); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestTraceCache304Golden,"VaRest.Http.Trace.Cache304Golden",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestTraceCache304Golden::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::TraceCacheOfflinePrivate;
    FTestRig Rig; FScopedTraceRecorder Trace(EVaRestTraceDetail::Metadata|EVaRestTraceDetail::Headers);
    Rig.Client.WithCache(FVaRestCachePolicy::Memory()); Rig.Fake->Enqueue(CacheResponse(TEXT("max-age=0")));
    const auto Seed=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/conditional")),Seed).Send();
    if (!TestTrue(TEXT("seed"),Rig.Wait(Seed))) return false;
    auto Reply=FVaRestFakeResponse::Text(304,TEXT("")); Reply.Headers.Set(TEXT("Cache-Control"),TEXT("max-age=60")); Rig.Fake->Enqueue(MoveTemp(Reply));
    const auto Result=MakeOutcome<FString>(); const auto Handle=Observe(Rig.Client.Get<FString>(TEXT("/conditional")),Result).Send();
    if (!TestTrue(TEXT("304 completed"),Rig.Wait(Result))) return false;
    TArray<FVaRestTraceEvent> Events; if (!Sequence(*this,*Trace.Recorder,Handle.GetId(),TEXT("Started,Dispatched,Ended,Note,Completed"),Events)) return false;
    if (!TestTrue(TEXT("actual attempt payloads"),Events.Num()==5 && Events[1].Kind==EVaRestTraceKind::AttemptDispatched && Events[2].Kind==EVaRestTraceKind::AttemptEnded)) return false;
    const auto& Dispatch=Events[1].Payload.Get<FVaRestTraceAttemptDispatched>(); const auto& Wire=Events[2].Payload.Get<FVaRestTraceAttemptEnded>();
    const auto& End=Events.Last().Payload.Get<FVaRestTraceCompleted>();
    const auto& Start=Events[0].Payload.Get<FVaRestTraceStarted>();
    TestTrue(TEXT("304 initial pending"),Start.bCacheLookupPending && Start.CacheLookup==EVaRestTraceCacheLookup::None);
    TestTrue(TEXT("conditional trace bit"),Dispatch.bConditional);
    TestEqual(TEXT("actual validator"),Dispatch.Headers.Get(TEXT("If-None-Match")),FString(TEXT("\"golden\"")));
    TestEqual(TEXT("wire 304"),Wire.Status,304); TestEqual(TEXT("dispatch attempt one"),Dispatch.Attempt,1); TestEqual(TEXT("ended attempt one"),Wire.Attempt,1);
    TestTrue(TEXT("nonnegative explicit waits"),Dispatch.LimiterWait>=0 && Dispatch.AuthWait>=0 && Dispatch.GapSeconds>=0);
    TestEqual(TEXT("no limiter phase fabricated"),Dispatch.LimiterWait,0.0); TestEqual(TEXT("no auth phase fabricated"),Dispatch.AuthWait,0.0);
    Note(*this,Events[3],Handle.GetId(),EVaRestTraceNoteKind::CacheRevalidated,1);
    TestTrue(TEXT("final revalidation"),End.CacheLookup==EVaRestTraceCacheLookup::Revalidate && End.bFromCache);
    TestEqual(TEXT("served status 200"),Result->Response.Status,200); TestEqual(TEXT("one attempt"),Result->Response.Attempts,1);
    TestEqual(TEXT("trace describes delivered 200, not physical 304"),End.Response.Status,200);
    TestEqual(TEXT("original-body entity size survives decode"),End.Response.BodyBytes,int64(13));
    TestTrue(TEXT("nonnegative response duration"),Result->Response.Elapsed>=0); TestEqual(TEXT("complete once"),Result->CompleteCalls.load(),1);
    if (!TestTrue(TEXT("body present"),Result->Value.IsSet())) return false;
    TestEqual(TEXT("original body"),Result->Value.GetValue(),FString(TEXT("original-body")));
    TestTrue(TEXT("drained"),Rig.Drain()); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestTraceCacheStaleGolden,"VaRest.Http.Trace.CacheStaleGolden",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestTraceCacheStaleGolden::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::TraceCacheOfflinePrivate;
    for (bool bAttempted : {false,true})
    {
        FTestRig Rig; FScopedTraceRecorder Trace; auto Policy=FVaRestCachePolicy::Memory(); Policy.bServeStaleOnError=true; Rig.Client.WithCache(Policy);
        Rig.Fake->Enqueue(CacheResponse(TEXT("max-age=0"))); const auto Seed=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/stale")),Seed).Send();
        if (!TestTrue(TEXT("seed"),Rig.Wait(Seed))) return false;
        if (bAttempted) { auto Failure=FVaRestFakeResponse::Text(0,TEXT("")); Failure.Failure=EVaRestTransportFailure::Connection; Rig.Fake->Enqueue(MoveTemp(Failure)); }
        else Rig.Connectivity->ReportOffline();
        const auto Result=MakeOutcome<FString>(); const auto Handle=Observe(Rig.Client.Get<FString>(TEXT("/stale")).NoRetry(),Result).Send();
        if (!TestTrue(TEXT("stale completed"),Rig.Wait(Result))) return false;
        TArray<FVaRestTraceEvent> Events; if (!Sequence(*this,*Trace.Recorder,Handle.GetId(),bAttempted?TEXT("Started,Dispatched,Ended,Note,Completed"):TEXT("Started,Note,Completed"),Events)) return false;
        const auto& End=Events.Last().Payload.Get<FVaRestTraceCompleted>();
        Note(*this,Events[Events.Num()-2],Handle.GetId(),EVaRestTraceNoteKind::CacheServedStale,bAttempted?1:0);
        TestTrue(TEXT("final stale provenance"),End.CacheLookup==EVaRestTraceCacheLookup::Stale && End.bFromCache);
        TestEqual(TEXT("actual attempt count"),Result->Response.Attempts,bAttempted?1:0);
        TestEqual(TEXT("physical calls"),Rig.Fake->NumCalls(),bAttempted?2:1); TestEqual(TEXT("no synthetic bytes"),Result->Response.BytesDown,int64(0));
        TestFalse(TEXT("cached success does not report online"),Rig.Connectivity->IsOnline());
        TestEqual(TEXT("one success"),Result->SuccessCalls.load(),1); TestEqual(TEXT("one complete"),Result->CompleteCalls.load(),1);
        TestTrue(TEXT("drained"),Rig.Drain());
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestTraceCacheDiskWriteAfterCompleted,"VaRest.Http.Trace.CacheDiskWriteAfterCompleted",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestTraceCacheDiskWriteAfterCompleted::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::TraceCacheOfflinePrivate;
    for (bool bRefuse : {false,true})
    {
        FScopedPersistenceTest Disk; FTestRig Rig; Storage(Rig,Disk); FScopedPersistenceHooks Hooks(Disk.Root); FScopedTraceRecorder Trace;
        const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->BeforeCacheWrite=Gate;
        ON_SCOPE_EXIT { Gate->Released=true; Rig.Client.CancelAll(); TestTrue(TEXT("disk write cleanup"),Rig.Drain()); };
        auto Policy=FVaRestCachePolicy::MemoryAndDisk(); Policy.MemoryBytes=0; Policy.DiskBytes=bRefuse?1:64*1024*1024; Rig.Client.WithCache(Policy);
        Rig.Fake->Enqueue(CacheResponse()); const auto Result=MakeOutcome<FString>(); const auto Handle=Observe(Rig.Client.Get<FString>(TEXT("/best-effort")),Result).Send();
        if (!TestTrue(TEXT("response precedes disk release"),Rig.Wait(Result))) return false;
        if (!TestTrue(TEXT("write held"),PumpUntil([Gate]{return Gate->Entered.load();}))) return false;
        TestFalse(TEXT("finished public handle"),Handle.IsActive()); TestEqual(TEXT("success even on disk refusal"),Result->SuccessCalls.load(),1);
        TArray<FVaRestTraceEvent> Events; if (!Sequence(*this,*Trace.Recorder,Handle.GetId(),TEXT("Started,Dispatched,Ended,Completed"),Events)) return false;
        Gate->Released=true; if (!TestTrue(TEXT("disk drained"),Rig.Drain())) return false;
        TestEqual(TEXT("no late note"),Trace.Recorder->For(Handle.GetId()).Num(),Events.Num()); TestFalse(TEXT("barrier did not timeout"),Gate->TimedOut.load());
        TestEqual(TEXT("application completed once"),Result->CompleteCalls.load(),1);
        const auto Hit=MakeOutcome<FString>(); if (bRefuse) Rig.Fake->Enqueue(CacheResponse());
        Observe(Rig.Client.Get<FString>(TEXT("/best-effort")),Hit).Send(); if (!TestTrue(TEXT("actual write disposition read back"),Rig.Wait(Hit) && Rig.Drain())) return false;
        TestEqual(TEXT("disk success versus refusal"),Hit->Response.bFromCache,!bRefuse);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestTraceOfflineQueuedGolden,"VaRest.Http.Trace.OfflineQueuedGolden",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestTraceOfflineQueuedGolden::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::TraceCacheOfflinePrivate;
    FScopedPersistenceTest Disk; FTestRig Rig; Storage(Rig,Disk); FScopedTraceRecorder Trace;
    Rig.Client.WithOfflineQueue(TEXT("queued-golden")); Rig.Connectivity->ReportOffline();
    const auto Queued=MakeShared<TArray<FString>,ESPMode::ThreadSafe>(); const auto Result=MakeOutcome<void>();
    const auto Handle=Observe(Rig.Client.Post<void>(TEXT("/write")).Body(TEXT("safe-body")).Idempotent().QueueWhenOffline()
        .OnQueued([Queued](const FString& Id){Queued->Add(Id);}),Result).Send();
    TestEqual(TEXT("OnQueued never inline"),Queued->Num(),0);
    if (!TestTrue(TEXT("durable acknowledgement"),PumpUntil([Queued]{return Queued->Num()==1;}))) return false;
    TArray<FVaRestTraceEvent> Events; if (!Sequence(*this,*Trace.Recorder,Handle.GetId(),TEXT("Started,Note,Completed"),Events)) return false;
    const auto& Start=Events[0].Payload.Get<FVaRestTraceStarted>(); const auto& End=Events.Last().Payload.Get<FVaRestTraceCompleted>();
    TestTrue(TEXT("queue flag"),EnumHasAnyFlags(Start.Flags,EVaRestTraceRequestFlags::QueueWhenOffline));
    TestEqual(TEXT("no cache policy"),Start.CachePolicy,FName(TEXT("None"))); TestFalse(TEXT("no pending cache"),Start.bCacheLookupPending);
    TestEqual(TEXT("no attempt entered"),Start.CurrentAttempt,0); Note(*this,Events[1],Handle.GetId(),EVaRestTraceNoteKind::Queued,0,(*Queued)[0]);
    TestEqual(TEXT("queued reason"),Events[1].Payload.Get<FVaRestTraceNote>().Reason,FName(TEXT("Offline")));
    TestTrue(TEXT("Queued completed"),End.Outcome==EVaRestTraceOutcome::Queued && End.CacheLookup==EVaRestTraceCacheLookup::None && !End.bFromCache);
    TestEqual(TEXT("original success absent"),Result->SuccessCalls.load(),0); TestEqual(TEXT("original error absent"),Result->ErrorCalls.load(),0); TestEqual(TEXT("original completion absent"),Result->CompleteCalls.load(),0);
    TestFalse(TEXT("handle inactive after ack"),Handle.IsActive()); TestEqual(TEXT("no wire"),Rig.Fake->NumCalls(),0);
    TestTrue(TEXT("drained"),Rig.Drain());
    const auto Late=MakeShared<FTraceRecorder,ESPMode::ThreadSafe>(); FVaRestTrace::AddSink(Late); ON_SCOPE_EXIT {FVaRestTrace::RemoveSink(Late);};
    const auto Snapshots=Late->GetSnapshots(); if (!TestEqual(TEXT("late snapshot"),Snapshots.Num(),1)) return false;
    TestFalse(TEXT("queued completed original absent from in-flight"),Snapshots[0].InFlight.ContainsByPredicate([Id=Handle.GetId()](const FVaRestTraceStarted& Row){return Row.RequestId==Id;}));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestTraceOfflineReplayGolden,"VaRest.Http.Trace.OfflineReplayGolden",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestTraceOfflineReplayGolden::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::TraceCacheOfflinePrivate;
    FScopedPersistenceTest Disk; FTestRig Rig; Storage(Rig,Disk); FScopedTraceRecorder Trace(EVaRestTraceDetail::Metadata|EVaRestTraceDetail::Headers);
    const auto Capture=MakeShared<FOuter,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
    ON_SCOPE_EXIT {Rig.Client.CancelAll(); Capture->Release(); TestTrue(TEXT("replay cleanup"),Rig.Drain());};
    Rig.Client.WithAuth(FVaRestAuth::Bearer()).WithCache(FVaRestCachePolicy::Memory()).WithOfflineQueue(TEXT("replay-golden"));
    Rig.Client.SetBearerToken(TEXT("old-long-secret")); Rig.Connectivity->SetProbeInterval(200); Rig.Connectivity->ReportOffline();
    const auto Queued=MakeShared<TArray<FString>,ESPMode::ThreadSafe>(); const auto Results=MakeShared<TArray<FVaRestQueuedResult>,ESPMode::ThreadSafe>(); const auto Original=MakeOutcome<void>();
    Rig.Client.OnQueuedResult().AddLambda([Results](const FVaRestQueuedResult& Reply){Results->Add(Reply);});
    const auto Handle=Observe(Rig.Client.Post<void>(TEXT("/write")).Body(TEXT("safe-body")).Idempotent().QueueWhenOffline()
        .OnQueued([Queued](const FString& Id){Queued->Add(Id);}),Original).Send();
    if (!TestTrue(TEXT("queued"),PumpUntil([Queued]{return Queued->Num()==1;}))) return false;
    // Queue days belong to the original logical interval, never a later replay's attempt phases.
    Rig.Clock->Advance(100); Rig.Client.SetBearerToken(TEXT("new-long-secret"),true);
    Rig.Fake->Enqueue(FVaRestFakeResponse::Manual()); Rig.Connectivity->ReportOnline();
    TestEqual(TEXT("result not inline on connectivity transition"),Results->Num(),0);
    if (!TestTrue(TEXT("replay on wire"),PumpUntil([&Rig]{return Rig.Fake->NumCalls()==1;}))) return false;
    if (!TestTrue(TEXT("captured actual replay"),Capture->Seen.IsValid())) return false;
    const uint64 ReplayId=Internals(*Capture->Seen).RequestId;
    TestFalse(TEXT("normal replay, not a timed probe"),Internals(*Capture->Seen).bConnectivityProbe);
    TestTrue(TEXT("fresh real request id"),ReplayId!=0 && ReplayId!=Handle.GetId());
    TestEqual(TEXT("current credentials on wire"),Rig.Fake->GetCall(0).Request.Headers.Get(TEXT("Authorization")),FString(TEXT("Bearer new-long-secret")));
    Rig.Clock->Advance(2); Rig.Fake->Complete(0,FVaRestFakeResponse::Text(200,TEXT("replayed")));
    if (!TestTrue(TEXT("durable retirement event"),PumpUntil([Results]{return Results->Num()==1;}))) return false;
    TArray<FVaRestTraceEvent> Events; if (!Sequence(*this,*Trace.Recorder,ReplayId,TEXT("Started,Dispatched,Ended,Completed"),Events)) return false;
    if (!TestTrue(TEXT("replay dispatch payload"),Events.Num()==4 && Events[1].Kind==EVaRestTraceKind::AttemptDispatched && Events[2].Kind==EVaRestTraceKind::AttemptEnded)) return false;
    const auto& Start=Events[0].Payload.Get<FVaRestTraceStarted>(); const auto& Dispatch=Events[1].Payload.Get<FVaRestTraceAttemptDispatched>(); const auto& End=Events.Last().Payload.Get<FVaRestTraceCompleted>();
    TestEqual(TEXT("exact durable trace link"),Start.QueueReplayOf.QueueId,(*Queued)[0]); TestEqual(TEXT("same-run original link"),Start.QueueReplayOf.OriginalRequestId,Handle.GetId());
    TestFalse(TEXT("replay not queue-enabled"),EnumHasAnyFlags(Start.Flags,EVaRestTraceRequestFlags::QueueWhenOffline));
    TestEqual(TEXT("replay cache disabled"),Start.CachePolicy,FName(TEXT("None"))); TestFalse(TEXT("replay no pending lookup"),Start.bCacheLookupPending);
    TestTrue(TEXT("replay final physical provenance"),End.CacheLookup==EVaRestTraceCacheLookup::None && !End.bFromCache);
    TestFalse(TEXT("replay not conditional"),Dispatch.bConditional); TestFalse(TEXT("trace redacts current credential"),Dispatch.Headers.Get(TEXT("Authorization")).Contains(TEXT("new-long-secret")));
    TestEqual(TEXT("replay limiter wait"),Dispatch.LimiterWait,0.0); TestEqual(TEXT("replay auth wait"),Dispatch.AuthWait,0.0); TestEqual(TEXT("replay gap"),Dispatch.GapSeconds,0.0);
    TestEqual(TEXT("replay logical time excludes queue days"),(*Results)[0].Response.Elapsed,2.0); TestEqual(TEXT("replay one attempt"),(*Results)[0].Response.Attempts,1);
    TestEqual(TEXT("retired exact item"),(*Results)[0].QueueId,(*Queued)[0]); TestTrue(TEXT("result success"),(*Results)[0].bSuccess);
    TestEqual(TEXT("retired before event"),Rig.Client.GetQueuedCount(),0); TestEqual(TEXT("original complete remains absent"),Original->CompleteCalls.load(),0);
    TestTrue(TEXT("drained"),Rig.Drain()); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestTraceOfflineExpiryGolden,"VaRest.Http.Trace.OfflineExpiryGolden",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestTraceOfflineExpiryGolden::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::TraceCacheOfflinePrivate;
    FScopedPersistenceTest Disk; FTestRig Rig; Storage(Rig,Disk); FScopedTraceRecorder Trace;
    const auto Capture=MakeShared<FOuter,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
    ON_SCOPE_EXIT {Rig.Client.CancelAll(); Capture->Release(); TestTrue(TEXT("expiry cleanup"),Rig.Drain());};
    FVaRestOfflinePolicy Policy; Policy.MaxAgeSeconds=5; Rig.Client.WithOfflineQueue(TEXT("expiry-golden"),Policy); Rig.Connectivity->ReportOffline();
    const auto Queued=MakeShared<TArray<FString>,ESPMode::ThreadSafe>(); const auto Results=MakeShared<TArray<FVaRestQueuedResult>,ESPMode::ThreadSafe>(); const auto Original=MakeOutcome<void>();
    Rig.Client.OnQueuedResult().AddLambda([Results](const FVaRestQueuedResult& Reply){Results->Add(Reply);});
    const auto Handle=Observe(Rig.Client.Put<void>(TEXT("/expired")).Body(TEXT("safe-body")).QueueWhenOffline()
        .OnQueued([Queued](const FString& Id){Queued->Add(Id);}),Original).Send();
    if (!TestTrue(TEXT("queued"),PumpUntil([Queued]{return Queued->Num()==1;}))) return false;
    Rig.Clock->Advance(6); Rig.Connectivity->ReportOnline();
    TestEqual(TEXT("not inline"),Results->Num(),0);
    if (!TestTrue(TEXT("expiry event"),PumpUntil([Results]{return Results->Num()==1;}))) return false;
    if (!TestTrue(TEXT("captured expiry exchange"),Capture->Seen.IsValid())) return false;
    const uint64 Id=Internals(*Capture->Seen).RequestId;
    TestTrue(TEXT("fresh synthetic replay id"),Id!=0 && Id!=Handle.GetId());
    TArray<FVaRestTraceEvent> Events; if (!Sequence(*this,*Trace.Recorder,Id,TEXT("Started,Note,Completed"),Events)) return false;
    const auto& Start=Events[0].Payload.Get<FVaRestTraceStarted>(); const auto& End=Events.Last().Payload.Get<FVaRestTraceCompleted>();
    TestEqual(TEXT("durable link"),Start.QueueReplayOf.QueueId,(*Queued)[0]); TestEqual(TEXT("original link"),Start.QueueReplayOf.OriginalRequestId,Handle.GetId());
    Note(*this,Events[1],Id,EVaRestTraceNoteKind::QueueExpired,0,(*Queued)[0]);
    TestTrue(TEXT("expiry final Offline"),End.Outcome==EVaRestTraceOutcome::Error && End.Error.Kind==EVaRestErrorKind::Offline);
    TestFalse(TEXT("not cached"),End.bFromCache); TestTrue(TEXT("lookup none"),End.CacheLookup==EVaRestTraceCacheLookup::None);
    TestEqual(TEXT("actual zero attempt"),Internals(*Capture->Seen).Attempt,0); TestEqual(TEXT("response zero attempts"),(*Results)[0].Response.Attempts,0);
    TestEqual(TEXT("response zero bytes"),(*Results)[0].Response.BytesDown,int64(0)); TestEqual(TEXT("zero wire"),Rig.Fake->NumCalls(),0);
    TestTrue(TEXT("retirement failure result"),!(*Results)[0].bSuccess && (*Results)[0].Error.Kind==EVaRestErrorKind::Offline);
    TestEqual(TEXT("retired once"),Rig.Client.GetQueuedCount(),0); TestEqual(TEXT("no original complete"),Original->CompleteCalls.load(),0);
    TestTrue(TEXT("drained"),Rig.Drain()); TestEqual(TEXT("one result"),Results->Num(),1); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestTraceCacheAttachWhileRead,"VaRest.Http.Trace.CacheAttachWhileRead",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestTraceCacheAttachWhileRead::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::TraceCacheOfflinePrivate;
    FScopedPersistenceTest Disk; FTestRig Rig; Storage(Rig,Disk); FScopedTraceRecorder Trace;
    auto Policy=FVaRestCachePolicy::MemoryAndDisk(); Policy.MemoryBytes=0; Rig.Client.WithCache(Policy);
    Rig.Fake->Enqueue(CacheResponse()); const auto Seed=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/held-read")),Seed).Send();
    if (!TestTrue(TEXT("seed disk"),Rig.Wait(Seed) && Rig.Drain())) return false;
    FScopedPersistenceHooks Hooks(Disk.Root); const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->BeforeRead=Gate;
    ON_SCOPE_EXIT {Gate->Released=true; Rig.Client.CancelAll(); TestTrue(TEXT("read cleanup"),Rig.Drain());};
    const auto Result=MakeOutcome<FString>(); const auto Handle=Observe(Rig.Client.Get<FString>(TEXT("/held-read")),Result).Send();
    if (!TestTrue(TEXT("held read entered"),PumpUntil([Gate]{return Gate->Entered.load();}))) return false;
    const auto OriginalEvents=Trace.Recorder->For(Handle.GetId());
    if (!TestTrue(TEXT("original Started"),OriginalEvents.Num()==1 && OriginalEvents[0].Kind==EVaRestTraceKind::Started)) return false;
    const auto Attached=MakeShared<FTraceRecorder,ESPMode::ThreadSafe>(); FVaRestTrace::AddSink(Attached); ON_SCOPE_EXIT {FVaRestTrace::RemoveSink(Attached);};
    Snapshot(*this,*Attached,Handle.GetId(),OriginalEvents[0].Payload.Get<FVaRestTraceStarted>().StartedAt,TEXT("MemoryAndDisk"),true,EVaRestTraceCacheLookup::None);
    TestEqual(TEXT("pending callback absent"),Result->CompleteCalls.load(),0);
    Gate->Released=true; if (!TestTrue(TEXT("read finished"),Rig.Wait(Result) && Rig.Drain())) return false;
    TestEqual(TEXT("attached tail only Completed"),Attached->KindsOf(Handle.GetId()),FString(TEXT("Completed")));
    const auto Tail=Attached->For(Handle.GetId()); if (!TestTrue(TEXT("final payload"),Tail.Num()==1 && Tail[0].Kind==EVaRestTraceKind::Completed)) return false;
    TestTrue(TEXT("final validated disk"),Tail[0].Payload.Get<FVaRestTraceCompleted>().CacheLookup==EVaRestTraceCacheLookup::HitDisk && Tail[0].Payload.Get<FVaRestTraceCompleted>().bFromCache);
    TestEqual(TEXT("zero attempts"),Result->Response.Attempts,0); TestEqual(TEXT("one completion"),Result->CompleteCalls.load(),1);
    TestFalse(TEXT("barrier not timed out"),Gate->TimedOut.load()); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestTraceCacheRepeatedOuter,"VaRest.Http.Trace.CacheRepeatedOuter",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestTraceCacheRepeatedOuter::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::TraceCacheOfflinePrivate;
    FScopedPersistenceTest Disk; FTestRig Rig; Storage(Rig,Disk); FScopedTraceRecorder Trace;
    auto Policy=FVaRestCachePolicy::MemoryAndDisk(); Policy.MemoryBytes=0; Rig.Client.WithCache(Policy);
    Rig.Fake->Enqueue(CacheResponse()); const auto Seed=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/repeated")),Seed).Send();
    if (!TestTrue(TEXT("seed disk"),Rig.Wait(Seed) && Rig.Drain())) return false;
    const auto State=FClientAccess::State(Rig.Client); const int64 BeforeLookups=State->GetCounters().CacheLookups.load(),BeforeHits=State->GetCounters().CacheHits.load();
    const auto Outer=MakeShared<FOuter,ESPMode::ThreadSafe>(); Outer->bRepeat=true; Rig.Client.Use(Outer,EVaRestStageSlot::Outer);
    FScopedPersistenceHooks Hooks(Disk.Root); const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
    ON_SCOPE_EXIT {Gate->Released=true; Rig.Client.CancelAll(); Outer->Release(); TestTrue(TEXT("repeat cleanup"),Rig.Drain());};
    const auto Result=MakeOutcome<FString>(); const auto Handle=Observe(Rig.Client.Get<FString>(TEXT("/repeated")),Result).Send();
    if (!TestTrue(TEXT("first inner pass"),PumpUntil([Outer]{return Outer->InnerCalls==1;}))) return false;
    if (!TestTrue(TEXT("actual exchange captured"),Outer->Seen.IsValid())) return false;
    TestTrue(TEXT("first validated disk disposition"),Internals(*Outer->Seen).CacheLookup==EVaRestTraceCacheLookup::HitDisk);
    const double StartedAt=Internals(*Outer->Seen).StartedAt;
    // The original logical completion permit remains owned: observe handbacks, never Drain a live held exchange.
    if (!TestTrue(TEXT("first read handed back"),PumpUntil([]{return PersistenceCounters().Launched.load()==PersistenceCounters().HandedBack.load();}))) return false;
    Hooks.Hooks->BeforeRead=Gate; Outer->Again();
    if (!TestTrue(TEXT("second read held"),PumpUntil([Gate]{return Gate->Entered.load();}))) return false;
    const auto Attached=MakeShared<FTraceRecorder,ESPMode::ThreadSafe>(); FVaRestTrace::AddSink(Attached); ON_SCOPE_EXIT {FVaRestTrace::RemoveSink(Attached);};
    Snapshot(*this,*Attached,Handle.GetId(),StartedAt,TEXT("MemoryAndDisk"),true,EVaRestTraceCacheLookup::None);
    TestTrue(TEXT("private current state reset"),Internals(*Outer->Seen).bCacheLookupPending && Internals(*Outer->Seen).CacheLookup==EVaRestTraceCacheLookup::None);
    Gate->Released=true; if (!TestTrue(TEXT("final pass completed"),Rig.Wait(Result) && Rig.Drain())) return false;
    TArray<FVaRestTraceEvent> Events; if (!Sequence(*this,*Trace.Recorder,Handle.GetId(),TEXT("Started,Completed"),Events)) return false;
    TestTrue(TEXT("final established disposition"),Events.Last().Payload.Get<FVaRestTraceCompleted>().CacheLookup==EVaRestTraceCacheLookup::HitDisk && Events.Last().Payload.Get<FVaRestTraceCompleted>().bFromCache);
    TestFalse(TEXT("final pending cleared"),Internals(*Outer->Seen).bCacheLookupPending);
    TestEqual(TEXT("one logical lookup across passes"),State->GetCounters().CacheLookups.load()-BeforeLookups,int64(1));
    TestEqual(TEXT("one logical hit across passes"),State->GetCounters().CacheHits.load()-BeforeHits,int64(1));
    TestTrue(TEXT("ratio bounded"),FHttpStats::CacheRatio(State->GetCounters())<=1.0);
    TestEqual(TEXT("two inner passes"),Outer->InnerCalls,2); TestEqual(TEXT("one application completion"),Result->CompleteCalls.load(),1);
    TestEqual(TEXT("no wire for either pass"),Rig.Fake->NumCalls(),1); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestTraceCacheUnresolvedCompletion,"VaRest.Http.Trace.CacheUnresolvedCompletion",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestTraceCacheUnresolvedCompletion::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::TraceCacheOfflinePrivate;
    // Cancel, early Outer response (with deliberately distinct provenance), actual owner loss.
    for (int32 Mode : {0,1,2})
    {
        FScopedPersistenceTest Disk; FTestRig Rig; Storage(Rig,Disk); FScopedTraceRecorder Trace;
        auto Policy=FVaRestCachePolicy::MemoryAndDisk(); Policy.MemoryBytes=0; Rig.Client.WithCache(Policy);
        FScopedPersistenceHooks Hooks(Disk.Root); const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->BeforeRead=Gate;
        const auto Outer=MakeShared<FOuter,ESPMode::ThreadSafe>(); Outer->bHold=Mode==1; Rig.Client.Use(Outer,EVaRestStageSlot::Outer);
        ON_SCOPE_EXIT {Gate->Released=true; Rig.Client.CancelAll(); Outer->Release(); TestTrue(TEXT("unresolved cleanup"),Rig.Drain());};
        UObject* Owner=NewObject<UVaRestHttpSettings>(GetTransientPackage(),NAME_None,RF_Transient);
        const auto Result=MakeOutcome<void>(); const auto Handle=Observe(Rig.Client.Get<void>(TEXT("/unresolved")).Owner(Owner),Result).Send();
        if (!TestTrue(TEXT("read held"),PumpUntil([Gate]{return Gate->Entered.load();}))) return false;
        if (!TestTrue(TEXT("captured live exchange"),Outer->Seen.IsValid())) return false;
        const auto Attached=MakeShared<FTraceRecorder,ESPMode::ThreadSafe>(); FVaRestTrace::AddSink(Attached); ON_SCOPE_EXIT {FVaRestTrace::RemoveSink(Attached);};
        Snapshot(*this,*Attached,Handle.GetId(),Internals(*Outer->Seen).StartedAt,TEXT("MemoryAndDisk"),true,EVaRestTraceCacheLookup::None);
        if (Mode==0) Handle.Cancel();
        else if (Mode==1) Outer->Answer(true);
        else {Owner->MarkAsGarbage(); Handle.Cancel();}
        if (!TestTrue(TEXT("completed before worker release"),PumpUntil([Outer]{return Internals(*Outer->Seen).bDone.load();}))) return false;
        TestFalse(TEXT("final unresolved pending cleared"),Internals(*Outer->Seen).bCacheLookupPending);
        TestTrue(TEXT("unresolved final lookup none"),Internals(*Outer->Seen).CacheLookup==EVaRestTraceCacheLookup::None);
        TArray<FVaRestTraceEvent> Events; if (!Sequence(*this,*Trace.Recorder,Handle.GetId(),TEXT("Started,Completed"),Events)) return false;
        const auto& End=Events.Last().Payload.Get<FVaRestTraceCompleted>();
        TestTrue(TEXT("final trace lookup none"),End.CacheLookup==EVaRestTraceCacheLookup::None);
        TestEqual(TEXT("actual Finish provenance independent of lookup"),End.bFromCache,Mode==1);
        TestTrue(TEXT("correct terminal kind"),End.Outcome==(Mode==1?EVaRestTraceOutcome::Success:EVaRestTraceOutcome::Canceled));
        TestEqual(TEXT("callback count including owner suppression"),Result->CompleteCalls.load(),Mode==2?0:1);
        Gate->Released=true; if (!TestTrue(TEXT("late read drained"),Rig.Drain())) return false;
        TestEqual(TEXT("no late metadata"),Trace.Recorder->For(Handle.GetId()).Num(),Events.Num()); TestEqual(TEXT("no late wire"),Rig.Fake->NumCalls(),0);
    }
    // A hit already established before an Outer-held outcome survives a later cancellation.
    FTestRig Rig; FScopedTraceRecorder Trace; Rig.Client.WithCache(FVaRestCachePolicy::Memory());
    Rig.Fake->Enqueue(CacheResponse()); const auto Seed=MakeOutcome<void>(); Observe(Rig.Client.Get<void>(TEXT("/established")),Seed).Send();
    if (!TestTrue(TEXT("memory seed"),Rig.Wait(Seed))) return false;
    const auto Outer=MakeShared<FOuter,ESPMode::ThreadSafe>(); Outer->bHold=true; Rig.Client.Use(Outer,EVaRestStageSlot::Outer);
    ON_SCOPE_EXIT {Rig.Client.CancelAll(); Outer->Release(); TestTrue(TEXT("established cleanup"),Rig.Drain());};
    const auto Result=MakeOutcome<void>(); const auto Handle=Observe(Rig.Client.Get<void>(TEXT("/established")),Result).Send();
    if (!TestTrue(TEXT("established inner hit"),Outer->Seen.IsValid() && Outer->InnerCalls==1)) return false;
    Handle.Cancel(); if (!TestTrue(TEXT("canceled established response"),Rig.Wait(Result))) return false;
    const auto Events=Trace.Recorder->For(Handle.GetId()); if (!TestTrue(TEXT("established end"),Events.Num()==2 && Events.Last().Kind==EVaRestTraceKind::Completed)) return false;
    TestTrue(TEXT("lookup history preserved"),Events.Last().Payload.Get<FVaRestTraceCompleted>().CacheLookup==EVaRestTraceCacheLookup::HitMemory);
    TestFalse(TEXT("cancellation does not deliver cached response"),Events.Last().Payload.Get<FVaRestTraceCompleted>().bFromCache); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestTraceQueueAttachWhilePrepare,"VaRest.Http.Trace.QueueAttachWhilePrepare",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestTraceQueueAttachWhilePrepare::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::TraceCacheOfflinePrivate;
    FScopedPersistenceTest Disk; FTestRig Rig; Storage(Rig,Disk); FScopedTraceRecorder Trace; FScopedPersistenceHooks Hooks(Disk.Root);
    const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->AfterPrepare=Gate;
    ON_SCOPE_EXIT {Gate->Released=true; Rig.Client.CancelAll(); TestTrue(TEXT("prepare cleanup"),Rig.Drain());};
    Rig.Client.WithOfflineQueue(TEXT("attach-queue")); Rig.Connectivity->ReportOffline();
    const auto Queued=MakeShared<TArray<FString>,ESPMode::ThreadSafe>(); const auto Result=MakeOutcome<void>();
    const auto Handle=Observe(Rig.Client.Put<void>(TEXT("/prepare")).Body(TEXT("safe-body")).QueueWhenOffline()
        .OnQueued([Queued](const FString& Id){Queued->Add(Id);}),Result).Send();
    if (!TestTrue(TEXT("prepare held"),PumpUntil([Gate]{return Gate->Entered.load();}))) return false;
    const auto Events=Trace.Recorder->For(Handle.GetId()); if (!TestTrue(TEXT("original start"),Events.Num()==1 && Events[0].Kind==EVaRestTraceKind::Started)) return false;
    const auto Attached=MakeShared<FTraceRecorder,ESPMode::ThreadSafe>(); FVaRestTrace::AddSink(Attached); ON_SCOPE_EXIT {FVaRestTrace::RemoveSink(Attached);};
    Snapshot(*this,*Attached,Handle.GetId(),Events[0].Payload.Get<FVaRestTraceStarted>().StartedAt,TEXT("None"),false,EVaRestTraceCacheLookup::None);
    const auto Snapshots=Attached->GetSnapshots(); if (!TestTrue(TEXT("snapshot row exists"),Snapshots.Num()==1 && Snapshots[0].InFlight.Num()==1)) return false;
    TestTrue(TEXT("current queue flag"),EnumHasAnyFlags(Snapshots[0].InFlight[0].Flags,EVaRestTraceRequestFlags::QueueWhenOffline));
    TestEqual(TEXT("snapshot zero attempt"),Snapshots[0].InFlight[0].CurrentAttempt,0);
    Rig.Clock->Advance(3); Gate->Released=true;
    if (!TestTrue(TEXT("queued after release"),PumpUntil([Queued]{return Queued->Num()==1;}) && Rig.Drain())) return false;
    TestEqual(TEXT("coherent attach tail"),Attached->KindsOf(Handle.GetId()),FString(TEXT("Note,Completed")));
    const auto Tail=Attached->For(Handle.GetId()); if (!TestTrue(TEXT("tail count"),Tail.Num()==2 && Tail[1].Kind==EVaRestTraceKind::Completed)) return false;
    Note(*this,Tail[0],Handle.GetId(),EVaRestTraceNoteKind::Queued,0,(*Queued)[0]);
    TestTrue(TEXT("queued tail"),Tail[1].Payload.Get<FVaRestTraceCompleted>().Outcome==EVaRestTraceOutcome::Queued);
    TestEqual(TEXT("outer logical queue time"),Tail[1].Time-Events[0].Time,3.0);
    TestEqual(TEXT("no original completion"),Result->CompleteCalls.load(),0); TestEqual(TEXT("no wire"),Rig.Fake->NumCalls(),0); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestTraceCacheQueueClosedOuter,"VaRest.Http.Trace.CacheQueueClosedOuter",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestTraceCacheQueueClosedOuter::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::TraceCacheOfflinePrivate;
    for (bool bQueue : {false,true})
    {
        FScopedPersistenceTest Disk; FTestRig Rig; Storage(Rig,Disk); FScopedTraceRecorder Trace; FScopedPersistenceHooks Hooks(Disk.Root);
        const auto Outer=MakeShared<FOuter,ESPMode::ThreadSafe>(); Outer->bHold=true; Rig.Client.Use(Outer,EVaRestStageSlot::Outer);
        const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
        ON_SCOPE_EXIT {Gate->Released=true; Rig.Client.CancelAll(); Outer->Release(); TestTrue(TEXT("closed Outer cleanup"),Rig.Drain());};
        const auto Result=MakeOutcome<void>(); const auto Queued=MakeShared<std::atomic<int32>,ESPMode::ThreadSafe>(0); FVaRestRequestHandle Handle;
        if (bQueue)
        {
            Hooks.Hooks->BeforeRename=Gate; Rig.Client.WithOfflineQueue(TEXT("closed-outer")); Rig.Connectivity->ReportOffline();
            Handle=Observe(Rig.Client.Put<void>(TEXT("/closed")).Body(TEXT("safe-body")).QueueWhenOffline().OnQueued([Queued](const FString&){++*Queued;}),Result).Send();
            if (!TestTrue(TEXT("irreversible rename held"),PumpUntil([Gate]{return Gate->Entered.load();}))) return false;
        }
        else
        {
            Rig.Client.WithCache(FVaRestCachePolicy::MemoryAndDisk()); Rig.Fake->bCompleteOnWorker=false; Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
            Handle=Observe(Rig.Client.Get<void>(TEXT("/closed")),Result).Send();
            if (!TestTrue(TEXT("wire exists"),PumpUntil([&Rig]{return Rig.Fake->NumCalls()==1;}))) return false;
            Rig.Fake->Complete(0,CacheResponse()); // real GT handback queued, Outer answers before that handback
        }
        Outer->Answer(); if (!TestTrue(TEXT("Outer finishes"),Rig.Wait(Result))) return false;
        TArray<FVaRestTraceEvent> Events;
        if (!Sequence(*this,*Trace.Recorder,Handle.GetId(),bQueue?TEXT("Started,Completed"):TEXT("Started,Dispatched,Completed"),Events)) return false;
        TestTrue(TEXT("actual Outer completion"),Events.Last().Payload.Get<FVaRestTraceCompleted>().Outcome==EVaRestTraceOutcome::Success);
        Gate->Released=true; if (!TestTrue(TEXT("late work drained"),Rig.Drain())) return false;
        TestEqual(TEXT("closed record unchanged"),Trace.Recorder->For(Handle.GetId()).Num(),Events.Num());
        TestEqual(TEXT("one application result"),Result->CompleteCalls.load(),1); TestEqual(TEXT("no late Queued callback"),Queued->load(),0);
        TestEqual(TEXT("inner settles once"),Outer->InnerCalls,1);
        if (bQueue) TestEqual(TEXT("actual committed work retained"),Rig.Client.GetQueuedCount(),1);
        else TestEqual(TEXT("late cache result not retained"),FClientAccess::State(Rig.Client)->GetMemoryCache().Num(),0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestTraceCacheQueueNoSinks,"VaRest.Http.Trace.CacheQueueNoSinks",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestTraceCacheQueueNoSinks::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::TraceCacheOfflinePrivate;
    FVaRestScopedSinkIsolation Isolation; FScopedPersistenceTest Disk; FTestRig Rig; Storage(Rig,Disk);
    const uint64 Before=FVaRestTrace::Counters().EventsBuilt.load();
    Rig.Client.WithCache(FVaRestCachePolicy::MemoryAndDisk()).WithOfflineQueue(TEXT("no-sinks"));
    Rig.Fake->Enqueue(CacheResponse());
    for (int32 Pass=0;Pass<2;++Pass)
    {const auto Reply=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/cache")),Reply).Send(); if (!TestTrue(TEXT("cache operation"),Rig.Wait(Reply) && Rig.Drain())) return false;}
    Rig.Connectivity->ReportOffline(); const auto Queued=MakeShared<std::atomic<int32>,ESPMode::ThreadSafe>(0);
    Rig.Client.Put<void>(TEXT("/write")).Body(TEXT("safe-body")).QueueWhenOffline().OnQueued([Queued](const FString&){++*Queued;}).Send();
    if (!TestTrue(TEXT("queue operation"),PumpUntil([Queued]{return Queued->load()==1;}))) return false;
    const auto Results=MakeShared<std::atomic<int32>,ESPMode::ThreadSafe>(0); Rig.Client.OnQueuedResult().AddLambda([Results](const FVaRestQueuedResult&){++*Results;});
    Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("ok"))); Rig.Connectivity->ReportOnline();
    if (!TestTrue(TEXT("replay operation"),PumpUntil([Results]{return Results->load()==1;}) && Rig.Drain())) return false;
    TestEqual(TEXT("stores build no events without sinks"),FVaRestTrace::Counters().EventsBuilt.load(),Before); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestTraceDeliveredCacheResponse, "VaRest.Http.Trace.DeliveredResponse.Cache",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestTraceDeliveredCacheResponse::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::TraceCacheOfflinePrivate;
    // Raw results consume Finish.Body, including the cache's owned entity, before Completed.
    enum class ECase { Memory, Disk, Revalidated, OfflineStale, AttemptedStale, EmptyUrl };
    for (ECase Case : {ECase::Memory, ECase::Disk, ECase::Revalidated, ECase::OfflineStale, ECase::AttemptedStale, ECase::EmptyUrl})
    {
        FScopedPersistenceTest Disk;
        FTestRig Rig;
        Storage(Rig, Disk);
        FScopedTraceRecorder Trace(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Headers);
        ON_SCOPE_EXIT { Rig.Client.CancelAll(); Rig.Drain(); };
        auto Policy = Case == ECase::Disk ? FVaRestCachePolicy::MemoryAndDisk() : FVaRestCachePolicy::Memory();
        if (Case == ECase::Disk) Policy.MemoryBytes = 0;
        Policy.bServeStaleOnError = true;
        Rig.Client.WithCache(Policy);
        if (Case == ECase::EmptyUrl) Rig.Client.Use(MakeShared<FClearCachedOutcomeUrl, ESPMode::ThreadSafe>(), EVaRestStageSlot::Outer);
        const bool bStale = Case == ECase::Revalidated || Case == ECase::OfflineStale || Case == ECase::AttemptedStale;
        auto Reply = FVaRestFakeResponse::Text(200, TEXT("plain-response"));
        Reply.Headers.Set(TEXT("Cache-Control"), bStale ? TEXT("max-age=0") : TEXT("max-age=60"));
        Reply.Headers.Set(TEXT("ETag"), TEXT("\"delivered\""));
        Rig.Fake->Enqueue(MoveTemp(Reply));
        const auto Seed = MakeOutcome<TArray<uint8>>();
        const uint64 SeedId = Observe(Rig.Client.Get<TArray<uint8>>(TEXT("/delivered")), Seed).Send().GetId();
        if (!TestTrue(TEXT("seed and persistence complete"), Rig.Wait(Seed) && Rig.Drain())) return false;
        const auto SeedEvents = Trace.Recorder->For(SeedId);
        if (!TestTrue(TEXT("seed has Completed"), !SeedEvents.IsEmpty() && SeedEvents.Last().Kind == EVaRestTraceKind::Completed)) return false;
        const auto& Network = SeedEvents.Last().Payload.Get<FVaRestTraceCompleted>();
        TestEqual(TEXT("network entity size survives raw move"), Network.Response.BodyBytes, int64(14));
        TestEqual(TEXT("network delivered status"), Network.Response.Status, 200);
        TestTrue(TEXT("seed raw value retained"), Seed->Value.IsSet() && Seed->Value->Num() == 14);
        if (Case == ECase::Revalidated)
        {
            auto NotModified = FVaRestFakeResponse::Text(304, TEXT(""));
            NotModified.Headers.Set(TEXT("Cache-Control"), TEXT("max-age=60"));
            Rig.Fake->Enqueue(MoveTemp(NotModified));
        }
        else if (Case == ECase::OfflineStale) Rig.Connectivity->ReportOffline();
        else if (Case == ECase::AttemptedStale)
        {
            auto Failed = FVaRestFakeResponse::Text(0, TEXT(""));
            Failed.Failure = EVaRestTransportFailure::Connection;
            Rig.Fake->Enqueue(MoveTemp(Failed));
        }
        const auto Result = MakeOutcome<TArray<uint8>>();
        const uint64 Id = Observe(Rig.Client.Get<TArray<uint8>>(TEXT("/delivered")).NoRetry(), Result).Send().GetId();
        if (!TestTrue(TEXT("delivered cache result"), Rig.Wait(Result))) return false;
        const auto Events = Trace.Recorder->For(Id);
        if (!TestTrue(TEXT("final event is completion"), !Events.IsEmpty() && Events.Last().Kind == EVaRestTraceKind::Completed)) return false;
        const auto& Delivered = Events.Last().Payload.Get<FVaRestTraceCompleted>();
        TestTrue(TEXT("cached success"), Delivered.Outcome == EVaRestTraceOutcome::Success && Delivered.bFromCache);
        TestEqual(TEXT("delivered status independent of physical end"), Delivered.Response.Status, 200);
        TestEqual(TEXT("delivered entity is fourteen bytes"), Delivered.Response.BodyBytes, int64(14));
        TestTrue(TEXT("cached entity storage"), Delivered.Response.Storage == EVaRestTraceResponseStorage::Memory);
        TestEqual(TEXT("cached content type"), Delivered.Response.ContentType, FString(TEXT("text/plain; charset=utf-8")));
        TestEqual(TEXT("cached wire bytes remain zero"), Result->Response.BytesDown, int64(0));
        TestEqual(TEXT("cached effective URL matches delivered result"), Delivered.Response.EffectiveUrl, Result->Response.EffectiveUrl);
        if (Case == ECase::EmptyUrl)
            TestTrue(TEXT("actually empty cached URL stays empty"), Delivered.Response.EffectiveUrl.IsEmpty());
        else
            TestEqual(TEXT("default cached URL stays present"), Delivered.Response.EffectiveUrl, FString(TEXT("https://api.test/delivered")));
        TestTrue(TEXT("result still owns its entity"), Result->Value.IsSet() && Result->Value->Num() == 14);
        int32 PhysicalEnds = 0;
        for (const auto& Event : Events)
        {
            if (Event.Kind != EVaRestTraceKind::AttemptEnded) continue;
            ++PhysicalEnds;
            const auto& Wire = Event.Payload.Get<FVaRestTraceAttemptEnded>();
            if (Case == ECase::Revalidated) TestEqual(TEXT("physical end is 304"), Wire.Status, 304);
            if (Case == ECase::AttemptedStale) TestTrue(TEXT("physical connection failure"), Wire.Failure == EVaRestTransportFailure::Connection);
        }
        TestEqual(TEXT("only attempted paths have an end"), PhysicalEnds,
            Case == ECase::Revalidated || Case == ECase::AttemptedStale ? 1 : 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestTraceDeliveredQueuedResponse, "VaRest.Http.Trace.DeliveredResponse.Queued",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestTraceDeliveredQueuedResponse::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::TraceCacheOfflinePrivate;
    FScopedPersistenceTest Disk;
    FTestRig Rig;
    Storage(Rig, Disk);
    FScopedTraceRecorder Trace;
    ON_SCOPE_EXIT { Rig.Client.CancelAll(); Rig.Drain(); };
    Rig.Client.WithOfflineQueue(TEXT("delivered-queued"));
    Rig.Connectivity->ReportOffline();
    const auto Queued = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
    const uint64 Id = Rig.Client.Post<void>(TEXT("/queued")).Body(TEXT("safe-body")).Idempotent().QueueWhenOffline()
        .OnQueued([Queued](const FString&) { ++*Queued; }).Send().GetId();
    if (!TestTrue(TEXT("durable queue acknowledgement"), PumpUntil([Queued] { return Queued->load() == 1; }))) return false;
    const auto Events = Trace.Recorder->For(Id);
    if (!TestTrue(TEXT("queued completion"), !Events.IsEmpty() && Events.Last().Kind == EVaRestTraceKind::Completed)) return false;
    const auto& End = Events.Last().Payload.Get<FVaRestTraceCompleted>();
    TestTrue(TEXT("queued outcome"), End.Outcome == EVaRestTraceOutcome::Queued);
    TestEqual(TEXT("queued has no delivered status"), End.Response.Status, 0);
    TestEqual(TEXT("queued size unobserved"), End.Response.BodyBytes, int64(-1));
    TestTrue(TEXT("queued storage unknown"), End.Response.Storage == EVaRestTraceResponseStorage::Unknown);
    TestTrue(TEXT("queued has no delivered metadata"), End.Response.Headers.IsEmpty() && End.Response.ContentType.IsEmpty() && End.Response.EffectiveUrl.IsEmpty());
    TestEqual(TEXT("queued has no wire attempt"), Rig.Fake->NumCalls(), 0);
    return true;
}

#endif
