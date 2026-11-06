// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS
#include "VaRestHttpTesting.h"
#include "VaRestQueueStore.h"
#include "VaRestClientState.h"
#include "VaRestIdentity.h"
#include "VaRestPersistence.h"
#include "VaRestAuth.h"
#include "VaRestExchangeState.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestReplayIdentityAndFifoTest,"VaRest.Http.Replay.IdentityAndFifo", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestReplayIdentityAndFifoTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
FTestRig Rig;
ON_SCOPE_EXIT {Rig.Client.CancelAll(); Rig.Drain();};
Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")), FPaths::Combine(Disk.Root,TEXT("Queue")));
Rig.Client.WithAuth(FVaRestAuth::Bearer()).WithOfflineQueue(TEXT("orders-account-A"));
Rig.Client.SetBearerToken(TEXT("A-old-token"));
Rig.Connectivity->ReportOffline();
FScopedTraceRecorder Trace(EVaRestTraceDetail::Metadata);
const auto Queued = MakeShared<TArray<FString>,ESPMode::ThreadSafe>();
for (int32 Number : {1,2})
{
    Rig.Client.Post<void>(TEXT("/orders")).Body(LexToString(Number)).Idempotent().QueueWhenOffline()
        .Endpoint(TEXT("ReplayEntry_17"))
        .OnQueued([Queued](const FString& Id) { Queued->Add(Id); }).Send();
}
if (!TestTrue(TEXT("two durable items"), PumpUntil([Queued] { return Queued->Num()==2; }))) return false;
const auto Results = MakeShared<TArray<FVaRestQueuedResult>,ESPMode::ThreadSafe>();
Rig.Client.OnQueuedResult().AddLambda([Results](const FVaRestQueuedResult& Result) { Results->Add(Result); });
Rig.Client.SetBearerToken(TEXT("A-new-token"), true);
Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
Rig.Connectivity->ReportOnline();
if (!TestTrue(TEXT("first starts"), PumpUntil([&Rig] { return Rig.Fake->NumCalls()==1; }))) return false;
if (!TestEqual(TEXT("one physical replay"), Rig.Fake->NumCalls(), 1)) return false;
TestEqual(TEXT("current auth applied"), Rig.Fake->GetCall(0).Request.Headers.Get(TEXT("Authorization")), FString(TEXT("Bearer A-new-token")));
PumpPersistence(); // the live replay retains its owned completion permit until terminal
TestEqual(TEXT("guard workers handed back"),PersistenceCounters().HandedBack.load(),PersistenceCounters().Launched.load());
TestEqual(TEXT("second waits"), Rig.Fake->NumCalls(), 1);
Rig.Fake->Complete(0, FVaRestFakeResponse::Text(200, TEXT("ok-one")));
if (!TestTrue(TEXT("second starts after durable removal"), PumpUntil([&Rig] { return Rig.Fake->NumCalls()==2; }))) return false;
Rig.Fake->Complete(1, FVaRestFakeResponse::Text(200, TEXT("ok-two")));
if (!TestTrue(TEXT("two results"), PumpUntil([Results] { return Results->Num()==2; }))) return false;
TestEqual(TEXT("first result id"), (*Results)[0].QueueId, (*Queued)[0]);
for (int32 Index=0;Index<2;++Index)
{
    const uint64 ReplayRequest=Rig.Fake->GetCall(Index).Request.RequestId;
    TestTrue(TEXT("actual replay Started retains endpoint and durable queue identity"),Trace.Recorder->For(ReplayRequest).ContainsByPredicate([&](const FVaRestTraceEvent& Event)
    {
        if (Event.Kind!=EVaRestTraceKind::Started) return false;
        const auto& Started=Event.Payload.Get<FVaRestTraceStarted>();
        return Started.Endpoint==FName(TEXT("ReplayEntry_17")) && Started.QueueReplayOf.QueueId.Equals((*Queued)[Index],ESearchCase::CaseSensitive);
    }));
}
TestEqual(TEXT("second result id"), (*Results)[1].QueueId, (*Queued)[1]);
TestEqual(TEXT("drained queue"), Rig.Client.GetQueuedCount(), 0);
TestTrue(TEXT("drained workers"), Rig.Drain());
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestReplayAccountAndTenantReplacementTest,"VaRest.Http.Replay.AccountAndTenantReplacement", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestReplayAccountAndTenantReplacementTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http::Testing;
for (bool bTenantOnly : {false,true})
{
    FScopedPersistenceTest Disk;
    FTestRig Rig;
ON_SCOPE_EXIT {Rig.Client.CancelAll(); Rig.Drain();};
    Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
        .WithAuth(FVaRestAuth::Bearer()).WithOfflineQueue(TEXT("identity-refusal"));
    Rig.Client.SetBearerToken(TEXT("account-A-token"));
    Rig.Client.SetSessionHeader(TEXT("X-Tenant"),TEXT("tenant-A"));
    Rig.Connectivity->ReportOffline();
    const auto Queued=MakeShared<FString,ESPMode::ThreadSafe>();
    const auto Results=MakeShared<TArray<FVaRestQueuedResult>,ESPMode::ThreadSafe>();
    Rig.Client.OnQueuedResult().AddLambda([Results](const FVaRestQueuedResult& Result){ Results->Add(Result); });
    Rig.Client.Post<void>(TEXT("/write")).Body(TEXT("safe")).Idempotent().QueueWhenOffline()
        .OnQueued([Queued](const FString& Id){ *Queued=Id; }).Send();
    if (!TestTrue(TEXT("accepted before identity mutation"),PumpUntil([Queued]{ return !Queued->IsEmpty(); }))) return false;
    if (bTenantOnly) Rig.Client.SetSessionHeader(TEXT("X-Tenant"),TEXT("tenant-B"));
    else Rig.Client.SetBearerToken(TEXT("account-B-token"));
    Rig.Connectivity->ReportOnline();
    if (!TestTrue(TEXT("terminal result"),PumpUntil([Results]{ return Results->Num()==1; }))) return false;
    TestTrue(TEXT("identity mismatch is Auth"),(*Results)[0].Error.Kind==EVaRestErrorKind::Auth);
    TestEqual(TEXT("zero cross-account wire calls"),Rig.Fake->NumCalls(),0);
    TestEqual(TEXT("durably retired before event"),Rig.Client.GetQueuedCount(),0);
    TestTrue(TEXT("drained"),Rig.Drain());
}
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestReplaySharedOwnerFailoverTest,"VaRest.Http.Replay.SharedOwnerFailover", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestReplaySharedOwnerFailoverTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
FTestRig First,Second;
ON_SCOPE_EXIT {First.Client.CancelAll(); Second.Client.CancelAll(); First.Drain(); Second.Drain();};
const auto Shared=First.Connectivity;
for (FVaRestClient* Client : {&First.Client,&Second.Client})
    Client->WithConnectivity(Shared).WithClock(First.Clock)
        .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
        .WithOfflineQueue(TEXT("shared-orders"));
Shared->ReportOffline();
const auto Queued=MakeShared<std::atomic<int32>,ESPMode::ThreadSafe>(0);
const auto FirstEvents=MakeShared<std::atomic<int32>,ESPMode::ThreadSafe>(0);
const auto SecondEvents=MakeShared<TArray<FString>,ESPMode::ThreadSafe>();
First.Client.OnQueuedResult().AddLambda([FirstEvents](const FVaRestQueuedResult&){ ++*FirstEvents; });
Second.Client.OnQueuedResult().AddLambda([SecondEvents](const FVaRestQueuedResult& Result){ SecondEvents->Add(Result.QueueId); });
for (const TCHAR* Body : {TEXT("one"),TEXT("two")})
    First.Client.Post<void>(TEXT("/write")).Body(Body).Idempotent().QueueWhenOffline()
        .OnQueued([Queued](const FString&){ ++*Queued; }).Send();
if (!TestTrue(TEXT("both queued"),PumpUntil([Queued]{ return Queued->load()==2; }))) return false;
First.Fake->Enqueue(FVaRestFakeResponse::Manual()); Second.Fake->Enqueue(FVaRestFakeResponse::Manual());
Shared->ReportOnline();
if (!TestTrue(TEXT("first owner selected"),PumpUntil([&First]{ return First.Fake->NumCalls()==1; }))) return false;
TestEqual(TEXT("second owner idle"),Second.Fake->NumCalls(),0);
First.Client=FVaRestClient(); // active exchange owns its state until its result finishes
First.Fake->Complete(0,FVaRestFakeResponse::Text(200,TEXT("one")));
if (!TestTrue(TEXT("next owner selected after retirement"),PumpUntil([&Second]{ return Second.Fake->NumCalls()==1; }))) return false;
Second.Fake->Complete(0,FVaRestFakeResponse::Text(200,TEXT("two")));
if (!TestTrue(TEXT("surviving subscriber sees both"),PumpUntil([SecondEvents]{ return SecondEvents->Num()==2; }))) return false;
TestTrue(TEXT("different durable ids"),!(*SecondEvents)[0].Equals((*SecondEvents)[1],ESearchCase::CaseSensitive));
TestEqual(TEXT("departed subscriber is not resurrected"),FirstEvents->load(),0);
TestEqual(TEXT("first wire exactly once"),First.Fake->NumCalls(),1);
TestEqual(TEXT("second wire exactly once"),Second.Fake->NumCalls(),1);
TestEqual(TEXT("empty shared queue"),Second.Client.GetQueuedCount(),0);
TestTrue(TEXT("drained"),First.Drain() && Second.Drain());
return true;
}

namespace VaRest::Http::ReplayTestPrivate
{
    struct FCleanup
    {
        Testing::FTestRig& Rig;
        explicit FCleanup(Testing::FTestRig& InRig):Rig(InRig){}
        ~FCleanup(){ Rig.Client.CancelAll(); Rig.Drain(); }
    };
    void Configure(Testing::FTestRig& Rig,const Testing::FScopedPersistenceTest& Disk,const TCHAR* Name=TEXT("replay"),FVaRestOfflinePolicy Policy={})
    {
        Rig.Connectivity->ReportOffline();
        Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue"))).WithOfflineQueue(Name,Policy);
    }
    class FCapture final : public IVaRestStage
    {
    public:
        TWeakPtr<FVaRestExchange,ESPMode::ThreadSafe> Seen; uint64 LastId=0;
        virtual FName GetStageName() const override { return TEXT("ReplayScopeCapture"); }
        virtual void Process(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done) override
        { Seen=Exchange; LastId=Internals(*Exchange).RequestId; Next.Proceed(Exchange,MoveTemp(Done)); }
    };
    class FCustomAuth final : public IVaRestAuth
    {
    public:
        FName Contract,Name=TEXT("RestartCustom");
        virtual FName GetAuthName() const override { return Name; }
        virtual FName GetIdentityContractName() const override { return Contract; }
        virtual bool GetIdentity(const FVaRestSessionData&,FString& Out) const override { Out=TEXT("stable-custom-account"); return true; }
        virtual bool Apply(FVaRestAuthTarget& Target) const override
        { Target.Headers.Set(TEXT("Authorization"),TEXT("Bearer ")+Target.Session.AccessToken); return true; }
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestReplayBlockedScopeTest,"VaRest.Http.Replay.BlockedScope",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestReplayBlockedScopeTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::ReplayTestPrivate;
    // Authenticated replay, anonymous replay origin, opted original origin, ordinary control, anonymous login control.
    for (int32 Mode=0;Mode<5;++Mode)
    {
        FScopedPersistenceTest Disk; FTestRig Rig; FCleanup Cleanup(Rig); FScopedTraceRecorder Trace;
        Rig.Client.WithAuth(FVaRestAuth::Bearer()).WithMaxConcurrent(1); Rig.Client.SetBearerToken(TEXT("account-A")); Configure(Rig,Disk);
        const auto Results=MakeShared<TArray<FVaRestQueuedResult>,ESPMode::ThreadSafe>();
        Rig.Client.OnQueuedResult().AddLambda([Results](const FVaRestQueuedResult& R){Results->Add(R);});
        const bool bReplay=Mode<2 || Mode==4;
        if (bReplay)
        {
            const auto Q=MakeShared<int32,ESPMode::ThreadSafe>(0);
            auto Request=Rig.Client.Put<void>(TEXT("/scoped")).Body(TEXT("safe")).QueueWhenOffline();
            if (Mode!=0) Request=MoveTemp(Request).NoAuth();
            MoveTemp(Request).OnQueued([Q](const FString&){++*Q;}).Send();
            if (!TestTrue(TEXT("initial durable item"),PumpUntil([Q]{return *Q==1;}))) return false;
        }
        Rig.Connectivity->ReportOnline(); // replay is scheduled, not entered inline
        Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
        auto Hold=MakeOutcome<void>(); Observe(Rig.Client.Get<void>(TEXT("/hold")).NoAuth(),Hold).Send();
        if (!TestEqual(TEXT("real occupied slot"),Rig.Fake->NumCalls(),1)) return false;
        const auto Capture=MakeShared<FCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
        const auto Original=MakeOutcome<void>();
        if (!bReplay)
        {
            auto Request=Rig.Client.Put<void>(TEXT("/scoped")).Body(TEXT("safe"));
            if (Mode==2) Request=MoveTemp(Request).NoAuth().QueueWhenOffline();
            Observe(MoveTemp(Request),Original).Send();
        }
        if (!TestTrue(TEXT("scope request entered limiter"),PumpUntil([Capture]{ const auto Ex=Capture->Seen.Pin(); return Ex.IsValid() && Internals(*Ex).bInLimiterQueue; }))) return false;
        const auto Ex=Capture->Seen.Pin(); if (!TestTrue(TEXT("exchange retained"),Ex.IsValid())) return false;
        const uint64 Id=Internals(*Ex).RequestId;
        if (Mode==1 || Mode==2) Rig.Client.WithBaseUrl(TEXT("https://other.test"));
        else Rig.Client.SetBearerToken(TEXT("account-B"));
        Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("ok"))); Rig.Fake->Complete(0,FVaRestFakeResponse::Text(200,TEXT("held")));
        if (bReplay)
        {
            // Origin replacement leaves the old event domain. Its pinned exchange still must terminate and retire.
            if (Mode==1) { if (!TestTrue(TEXT("old pinned replay finishes"),Rig.Drain())) return false; }
            else if (!TestTrue(TEXT("queued result"),PumpUntil([Results]{return Results->Num()==1;}))) return false;
        }
        else if (!TestTrue(TEXT("original outcome"),Rig.Wait(Original))) return false;
        const bool bAllowed=Mode>=3;
        TestEqual(TEXT("late guard wire boundary"),Rig.Fake->NumCalls(),bAllowed?2:1);
        if (Mode==0) TestTrue(TEXT("blocked replay login is Auth"),(*Results)[0].Error.Kind==EVaRestErrorKind::Auth);
        if (Mode==2) TestTrue(TEXT("original anchor invalidation is Auth"),Original->Error.Kind==EVaRestErrorKind::Auth);
        if (Mode==3) TestEqual(TEXT("ordinary request uses current login"),Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("Authorization")),FString(TEXT("Bearer account-B")));
        if (Mode==4) TestFalse(TEXT("anonymous replay omits current login"),Rig.Fake->GetCall(1).Request.Headers.Contains(TEXT("Authorization")));
        int32 Dispatches=0; bool bConfig=false; for (const auto& Event:Trace.Recorder->For(Id))
        {
            if (Event.Kind==EVaRestTraceKind::AttemptDispatched) ++Dispatches;
            if (Event.Kind==EVaRestTraceKind::Completed) bConfig=Event.Payload.Get<FVaRestTraceCompleted>().Error.Kind==EVaRestErrorKind::Config;
        }
        if (Mode==1) TestTrue(TEXT("anonymous replay origin remains Config"),bConfig);
        TestEqual(TEXT("trace has no false dispatch on late refusal"),Dispatches,bAllowed?1:0);
        TestTrue(TEXT("drained"),Rig.Drain());
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestReplayCacheNoneTest,"VaRest.Http.Replay.CacheNone",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestReplayCacheNoneTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::ReplayTestPrivate;
    for (bool bHead : {false,true})
    {
        FScopedPersistenceTest Disk; FTestRig Rig; FCleanup Cleanup(Rig); Configure(Rig,Disk);
        const auto Q=MakeShared<int32,ESPMode::ThreadSafe>(0),Events=MakeShared<int32,ESPMode::ThreadSafe>(0);
        auto Original=bHead?Rig.Client.Head<void>(TEXT("/same")):Rig.Client.Get<void>(TEXT("/same"));
        MoveTemp(Original).QueueWhenOffline().OnQueued([Q](const FString&){++*Q;}).Send();
        if (!TestTrue(TEXT("queued read"),PumpUntil([Q]{return *Q==1;}))) return false;
        Rig.Client.WithOfflineQueue(TEXT(""));
        if (!TestTrue(TEXT("old registration released"),Disk.Drain())) return false;
        Rig.Client.WithCache(FVaRestCachePolicy::Memory());
        Rig.Connectivity->ReportOnline();
        auto Seed=FVaRestFakeResponse::Text(200,TEXT("seed")); Seed.Headers.Set(TEXT("Cache-Control"),TEXT("max-age=600"));
        Rig.Fake->Enqueue(MoveTemp(Seed)); const auto A=MakeOutcome<void>();
        Observe(bHead?Rig.Client.Head<void>(TEXT("/same")):Rig.Client.Get<void>(TEXT("/same")),A).Send();
        if (!TestTrue(TEXT("cache seed"),Rig.Wait(A))) return false;
        Rig.Connectivity->ReportOffline(); Rig.Client.WithOfflineQueue(TEXT("replay"));
        if (!TestTrue(TEXT("reopen with populated cache"),Disk.Drain())) return false;
        Rig.Client.OnQueuedResult().AddLambda([Events](const FVaRestQueuedResult& R){if (R.bSuccess && !R.Response.bFromCache) ++*Events;});
        Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("replay"))); Rig.Connectivity->ReportOnline();
        if (!TestTrue(TEXT("replay uses physical wire despite fresh hit"),PumpUntil([Events]{return *Events==1;}))) return false;
        if (!TestEqual(TEXT("seed plus one nonprobe replay"),Rig.Fake->NumCalls(),2)) return false;
        auto B=MakeOutcome<void>(); Observe(bHead?Rig.Client.Head<void>(TEXT("/same")):Rig.Client.Get<void>(TEXT("/same")),B).Send();
        if (!TestTrue(TEXT("ordinary hit retained"),Rig.Wait(B))) return false;
        TestTrue(TEXT("ordinary hit"),B->Response.bFromCache); TestEqual(TEXT("no cache replacement lookup wire"),Rig.Fake->NumCalls(),2);
        TestEqual(TEXT("replay bypassed lookup accounting"),FClientAccess::State(Rig.Client)->GetCounters().CacheLookups.load(),int64(2));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestReplayRestartTest,"VaRest.Http.Replay.Restart",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestReplayRestartTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::ReplayTestPrivate;
    // Builtin restored A, different session, retained default custom, replacement custom, declared replacement custom.
    for (int32 Mode=0;Mode<5;++Mode)
    {
        FScopedPersistenceTest Disk; const auto Provider=MakeShared<FCustomAuth,ESPMode::ThreadSafe>();
        if (Mode==4) Provider->Contract=TEXT("RestartContract.v1");
        FString Exported,QueueId,Key; uint64 OriginalId=0;
        const auto Original=MakeOutcome<void>();
        {
            FTestRig First; FCleanup Cleanup(First);
            First.Client.WithAuth(Mode<2?FVaRestAuth::Bearer():Provider); First.Client.SetBearerToken(TEXT("A-old"));
            Configure(First,Disk); Exported=First.Client.ExportSession(); const auto Q=MakeShared<FString,ESPMode::ThreadSafe>();
            const auto H=Observe(First.Client.Put<void>(TEXT("/restart")).Body(TEXT("safe")).QueueWhenOffline(),Original).OnQueued([Q](const FString& Id){*Q=Id;}).Send();
            OriginalId=H.GetId(); if (!TestTrue(TEXT("first accepted"),PumpUntil([Q]{return !Q->IsEmpty();}))) return false; QueueId=*Q;
            const auto Flushed=MakeShared<bool,ESPMode::ThreadSafe>(false); First.Client.FlushQueue([Flushed](bool bOk){*Flushed=bOk;});
            if (!TestTrue(TEXT("flush before destroy"),PumpUntil([Flushed]{return *Flushed;}))) return false;
        }
        if (!TestTrue(TEXT("coordinator gone"),Disk.Drain())) return false;
        const FString Directory=QueueDirectory(FPaths::Combine(Disk.Root,TEXT("Queue")),TEXT("replay"));
        const auto Loaded=MakeShared<FQueueLoad,ESPMode::ThreadSafe>(); FQueueStore::Load(Directory,TEXT("https://api.test"),{},[Loaded](FQueueLoad&& R){*Loaded=MoveTemp(R);});
        if (!TestTrue(TEXT("inspect persisted record"),Disk.Drain()) || !TestEqual(TEXT("one record"),Loaded->Items.Num(),1)) return false;
        Key=Loaded->Items[0].IdempotencyKey;
        // A different process run is represented by persisted provenance, without changing production globals.
        auto Item=MoveTemp(Loaded->Items[0]); Item.OriginalRun=FGuid::NewGuid();
        const auto Rewritten=MakeShared<bool,ESPMode::ThreadSafe>(false);
        LaunchPersistence([Directory,Item=MoveTemp(Item)]() mutable {const FString Path=FPaths::Combine(Directory,QueueFileName(Item.Sequence,Item.QueueId)); auto Removed=DeleteFile(Path); return Removed.bOk?WriteNewFile(Path,EncodeQueueRecord(Item)):Removed;},[Rewritten](FIoResult&& R){*Rewritten=R.bOk;});
        if (!TestTrue(TEXT("different run persisted"),Disk.Drain()) || !TestTrue(TEXT("rewrite succeeded"),*Rewritten)) return false;
        FTestRig Second; FCleanup Cleanup(Second); FScopedTraceRecorder Trace;
        const auto Capture=MakeShared<FCapture,ESPMode::ThreadSafe>(); Second.Client.Use(Capture,EVaRestStageSlot::Outer);
        TSharedPtr<IVaRestAuth,ESPMode::ThreadSafe> Auth=Mode<2?FVaRestAuth::Bearer():Provider;
        if (Mode>=3) { auto New=MakeShared<FCustomAuth,ESPMode::ThreadSafe>(); New->Contract=FName(*Provider->Contract.ToString().ToLower()); New->Name=TEXT("restartcustom"); Auth=New; }
        Second.Client.WithAuth(Auth.ToSharedRef()); if (!TestTrue(TEXT("previously exported A imported"),Second.Client.ImportSession(Exported))) return false;
        Second.Client.SetBearerToken(Mode==1?TEXT("different-account"):TEXT("A-restored-current"),Mode!=1);
        Configure(Second,Disk); const auto Results=MakeShared<TArray<FVaRestQueuedResult>,ESPMode::ThreadSafe>();
        Second.Client.OnQueuedResult().AddLambda([Results](const FVaRestQueuedResult& R){Results->Add(R);});
        Second.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("ok"))); Second.Connectivity->ReportOnline();
        if (!TestTrue(TEXT("restart terminal result"),PumpUntil([Results]{return Results->Num()==1;}))) return false;
        const bool bSuccess=Mode!=1 && Mode!=3;
        TestEqual(TEXT("declared continuity or terminal identity refusal"),(*Results)[0].bSuccess,bSuccess);
        TestEqual(TEXT("new opaque custom instance never replays old head"),Second.Fake->NumCalls(),bSuccess?1:0);
        TestEqual(TEXT("durable queue id unchanged"),(*Results)[0].QueueId,QueueId);
        if (bSuccess)
        {
            TestEqual(TEXT("stable idempotency key"),Second.Fake->GetCall(0).Request.Headers.Get(TEXT("Idempotency-Key")),Key);
            TestEqual(TEXT("restored current credentials"),Second.Fake->GetCall(0).Request.Headers.Get(TEXT("Authorization")),FString(TEXT("Bearer A-restored-current")));
        }
        else TestTrue(TEXT("restart refusal is Auth"),(*Results)[0].Error.Kind==EVaRestErrorKind::Auth);
        bool bStarted=false; for (const auto& Event:Trace.Recorder->For(Capture->LastId))
            if (Event.Kind==EVaRestTraceKind::Started && Event.Payload.Get<FVaRestTraceStarted>().QueueReplayOf.QueueId==QueueId)
            { bStarted=true; TestEqual(TEXT("different run loses original numeric id"),Event.Payload.Get<FVaRestTraceStarted>().QueueReplayOf.OriginalRequestId,uint64(0)); }
        TestTrue(TEXT("replay provenance emitted"),bStarted); TestTrue(TEXT("original id existed"),OriginalId!=0);
        TestEqual(TEXT("old original callbacks absent"),Original->CompleteCalls.load(),0);
        TestEqual(TEXT("retired before event"),Second.Client.GetQueuedCount(),0);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestReplayFailureAndAgeTest,"VaRest.Http.Replay.FailureAndAge",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestReplayFailureAndAgeTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::ReplayTestPrivate;
    for (bool bExpiry : {false,true})
    {
        FScopedPersistenceTest Disk; FTestRig Rig; FCleanup Cleanup(Rig); FVaRestOfflinePolicy Policy; Policy.MaxAgeSeconds=bExpiry?10:100;
        Configure(Rig,Disk,TEXT("failure"),Policy); const auto Q=MakeShared<int32,ESPMode::ThreadSafe>(0); const auto Events=MakeShared<TArray<FVaRestQueuedResult>,ESPMode::ThreadSafe>();
        Rig.Client.OnQueuedResult().AddLambda([Events](const FVaRestQueuedResult& R){Events->Add(R);});
        for (const TCHAR* Path:{TEXT("/first"),TEXT("/second")}) Rig.Client.Put<void>(Path).Body(TEXT("safe")).QueueWhenOffline().OnQueued([Q](const FString&){++*Q;}).Send();
        if (!TestTrue(TEXT("two queued"),PumpUntil([Q]{return *Q==2;}))) return false;
        if (bExpiry) Rig.Clock->Advance(10);
        Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection)); Rig.Connectivity->ReportOnline();
        if (bExpiry)
        {
            if (!TestTrue(TEXT("exact age retires both"),PumpUntil([Events]{return Events->Num()==2;}))) return false;
            TestEqual(TEXT("expiry no wire"),Rig.Fake->NumCalls(),0); TestTrue(TEXT("terminal expiry Offline"),(*Events)[0].Error.Kind==EVaRestErrorKind::Offline);
        }
        else
        {
            if (!TestTrue(TEXT("first failure finished"),PumpUntil([&Rig]{return Rig.Fake->NumCalls()==1 && Rig.Client.GetInFlightCount()==0;}))) return false;
            TestEqual(TEXT("connection keeps both"),Rig.Client.GetQueuedCount(),2); TestTrue(TEXT("no terminal event"),Events->IsEmpty());
            const FString Key=Rig.Fake->GetCall(0).Request.Headers.Get(TEXT("Idempotency-Key")); Rig.Connectivity->ReportOffline();
            Rig.Clock->Advance(9); if (!TestTrue(TEXT("quiet before ten seconds"),Disk.Drain())) return false; TestEqual(TEXT("second suppressed"),Rig.Fake->NumCalls(),1);
            Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("first"))); Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("second"))); Rig.Clock->Advance(1);
            if (!TestTrue(TEXT("head retry resumes FIFO"),PumpUntil([Events]{return Events->Num()==2;}))) return false;
            TestEqual(TEXT("one failure plus two successful calls"),Rig.Fake->NumCalls(),3);
            TestTrue(TEXT("successful physical probe restores online"),Rig.Connectivity->IsOnline());
            TestEqual(TEXT("head same stable key"),Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("Idempotency-Key")),Key);
            TestTrue(TEXT("second after first"),Rig.Fake->GetCall(2).Request.Url.EndsWith(TEXT("/second")));
        }
        TestEqual(TEXT("empty after terminals"),Rig.Client.GetQueuedCount(),0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestReplaySharedReentrantTest,"VaRest.Http.Replay.SharedReentrant",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestReplaySharedReentrantTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::ReplayTestPrivate;
    FScopedPersistenceTest Disk; FTestRig First,Second; FCleanup CleanA(First),CleanB(Second);
    Second.Client.WithConnectivity(First.Connectivity).WithClock(First.Clock); Configure(First,Disk); Configure(Second,Disk);
    auto Copy=Second.Client; const auto Q=MakeShared<int32,ESPMode::ThreadSafe>(0);
    for (const TCHAR* Path:{TEXT("/first"),TEXT("/second")}) First.Client.Put<void>(Path).Body(TEXT("safe")).QueueWhenOffline().OnQueued([Q](const FString&){++*Q;}).Send();
    if (!TestTrue(TEXT("initial two durable"),PumpUntil([Q]{return *Q==2;}))) return false;
    const auto A=MakeShared<TArray<FString>,ESPMode::ThreadSafe>(),B=MakeShared<TArray<FString>,ESPMode::ThreadSafe>();
    const auto Reentrant=MakeShared<int32,ESPMode::ThreadSafe>(0); const auto Reachability=First.Connectivity;
    const auto Add=[Reachability,Reentrant](FVaRestClient& Client,const TSharedRef<TArray<FString>,ESPMode::ThreadSafe>& Seen,const TCHAR* Path)
    {
        const TWeakPtr<FClientState,ESPMode::ThreadSafe> Weak=FClientAccess::State(Client); const FString OwnedPath(Path);
        Client.OnQueuedResult().AddLambda([Weak,Seen,Reachability,Reentrant,OwnedPath](const FVaRestQueuedResult& R)
        {
            Seen->Add(R.QueueId);
            if (Seen->Num()==1) if (const auto State=Weak.Pin())
            {
                Reachability->ReportOffline();
                FClientAccess::Wrap(State).Put<void>(OwnedPath).Body(TEXT("new")).QueueWhenOffline().OnQueued([Reentrant](const FString&){++*Reentrant;}).Send();
            }
        });
    };
    Add(First.Client,A,TEXT("/third")); Add(Copy,B,TEXT("/fourth"));
    First.Fake->SetResponder([](const FVaRestTransportRequest&){return FVaRestFakeResponse::Text(200,TEXT("ok"));});
    Reachability->ReportOnline();
    if (!TestTrue(TEXT("both live subscribers recursively enqueue"),PumpUntil([Reentrant]{return *Reentrant==2;}))) return false;
    TestEqual(TEXT("one event per original client despite copy"),A->Num(),1); TestEqual(TEXT("second subscriber once"),B->Num(),1);
    Reachability->ReportOnline();
    if (!TestTrue(TEXT("all four terminal events"),PumpUntil([A,B]{return A->Num()==4 && B->Num()==4;}))) return false;
    TestTrue(TEXT("same shared event order"),*A==*B); TestEqual(TEXT("lowest id alone owns wire"),First.Fake->NumCalls(),4); TestEqual(TEXT("peer never sends duplicate"),Second.Fake->NumCalls(),0);
    if (!TestEqual(TEXT("four physical records"),First.Fake->NumCalls(),4)) return false;
    TestTrue(TEXT("old second stays ahead of recursive writes"),First.Fake->GetCall(1).Request.Url.EndsWith(TEXT("/second")));
    TestTrue(TEXT("first subscriber next"),First.Fake->GetCall(2).Request.Url.EndsWith(TEXT("/third")));
    TestTrue(TEXT("second subscriber last"),First.Fake->GetCall(3).Request.Url.EndsWith(TEXT("/fourth")));
    TestEqual(TEXT("empty shared coordinator"),Copy.GetQueuedCount(),0); return true;
}


namespace VaRest::Http::ReplayTestPrivate
{
    class FReplayRefreshAuth final : public IVaRestAuth
    {
    public:
        bool bOffline=false; int32 Calls=0;
        virtual FName GetAuthName() const override {return TEXT("ReplayRefresh");}
        virtual FName GetIdentityContractName() const override {return TEXT("ReplayRefresh.v1");}
        virtual bool GetIdentity(const FVaRestSessionData& Session,FString& Out) const override {Out=SessionScope(Session);return true;}
        virtual bool Apply(FVaRestAuthTarget& Target) const override {Target.Headers.Set(TEXT("Authorization"),TEXT("Bearer ")+Target.Session.AccessToken);return true;}
        virtual bool CanRefresh(const FVaRestSessionData&) const override {return true;}
        virtual void Refresh(const FVaRestAuthRefreshContext&,TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done) override
        {
            ++Calls;
            if (bOffline) Done(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Offline,TEXT("refresh offline")));
            else Done(FVaRestAuthRefreshResult::Refreshed(TEXT("A-refreshed-current"),TEXT(""),3600));
        }
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestReplayRefreshTest,"VaRest.Http.Replay.Refresh",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestReplayRefreshTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::ReplayTestPrivate;
    for (bool bOffline : {false,true})
    {
        FScopedPersistenceTest Disk; FTestRig Rig; FCleanup Cleanup(Rig);
        const auto Auth=MakeShared<FReplayRefreshAuth,ESPMode::ThreadSafe>(); Auth->bOffline=bOffline;
        Rig.Client.WithAuth(Auth); Rig.Client.SetBearerToken(TEXT("A-old")); Configure(Rig,Disk);
        const auto Q=MakeShared<FString,ESPMode::ThreadSafe>(); const auto Results=MakeShared<TArray<FVaRestQueuedResult>,ESPMode::ThreadSafe>();
        Rig.Client.OnQueuedResult().AddLambda([Results](const FVaRestQueuedResult& R){Results->Add(R);});
        Rig.Client.Post<void>(TEXT("/refresh-replay")).Body(TEXT("safe-payload")).Idempotent().QueueWhenOffline().OnQueued([Q](const FString& Id){*Q=Id;}).Send();
        if (!TestTrue(TEXT("durable before replay refresh"),PumpUntil([Q]{return !Q->IsEmpty();}))) return false;
        Rig.Fake->Enqueue(FVaRestFakeResponse::Text(401,TEXT("expired")));
        if (!bOffline) Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("ok")));
        Rig.Connectivity->ReportOnline();
        if (bOffline)
        {
            if (!TestTrue(TEXT("failed refresh completes replay attempt"),PumpUntil([&Rig,Auth]{return Auth->Calls==1 && Rig.Client.GetInFlightCount()==0;}))) return false;
            TestEqual(TEXT("refresh Offline retains exact head"),Rig.Client.GetQueuedCount(),1); TestTrue(TEXT("retained head emits no terminal event"),Results->IsEmpty());
            TestEqual(TEXT("no second physical attempt while refresh offline"),Rig.Fake->NumCalls(),1);
        }
        else
        {
            if (!TestTrue(TEXT("refreshed replay terminal"),PumpUntil([Results]{return Results->Num()==1;}))) return false;
            TestTrue(TEXT("same identity refresh succeeds"),(*Results)[0].bSuccess); TestEqual(TEXT("same durable id"),(*Results)[0].QueueId,*Q);
            if (!TestEqual(TEXT("401 and refreshed physical attempts"),Rig.Fake->NumCalls(),2)) return false;
            TestEqual(TEXT("old first credential"),Rig.Fake->GetCall(0).Request.Headers.Get(TEXT("Authorization")),FString(TEXT("Bearer A-old")));
            TestEqual(TEXT("current refreshed credential"),Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("Authorization")),FString(TEXT("Bearer A-refreshed-current")));
            TestEqual(TEXT("stable key through refresh replay"),Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("Idempotency-Key")),Rig.Fake->GetCall(0).Request.Headers.Get(TEXT("Idempotency-Key")));
            TestTrue(TEXT("body retained through refresh"),Rig.Fake->GetCall(1).Request.Body==Rig.Fake->GetCall(0).Request.Body);
            TestEqual(TEXT("retired after success"),Rig.Client.GetQueuedCount(),0);
        }
        TestEqual(TEXT("one refresh cycle"),Auth->Calls,1); TestTrue(TEXT("terminal work drains"),Rig.Drain());
    }
    return true;
}

#endif
