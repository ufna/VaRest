// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS
#include "VaRestHttpTesting.h"
#include "VaRestQueueStore.h"
#include "VaRestClientState.h"
#include "VaRestIdentity.h"
#include "VaRestPersistence.h"
#include "VaRestAuth.h"
#include "VaRestHttpSettings.h"
#include "UObject/Package.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineQueuedOnlyAfterCommitTest,"VaRest.Http.Offline.QueuedOnlyAfterCommit", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineQueuedOnlyAfterCommitTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http::Testing;
FTestRig Rig;
ON_SCOPE_EXIT {Rig.Client.CancelAll(); Rig.Drain();};
FScopedPersistenceTest Disk;
Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")), FPaths::Combine(Disk.Root,TEXT("Queue")));
Rig.Client.WithOfflineQueue(TEXT("account-A"));
Rig.Connectivity->ReportOffline();
const auto Original = MakeOutcome<void>();
const auto Queued = MakeShared<TArray<FString>,ESPMode::ThreadSafe>();
const FVaRestRequestHandle Handle = Observe(Rig.Client.Post<void>(TEXT("/write"))
    .Body(TEXT("safe-body")).Idempotent().QueueWhenOffline(), Original)
    .OnQueued([Queued](const FString& QueueId) { Queued->Add(QueueId); }).Send();
TestEqual(TEXT("not reentrant"), Queued->Num(), 0);
if (!TestTrue(TEXT("persisted acknowledgement"), PumpUntil([Queued] { return Queued->Num() == 1; }))) return false;
TestFalse(TEXT("original handle ended"), Handle.IsActive());
TestEqual(TEXT("no success"), Original->SuccessCalls.load(), 0);
TestEqual(TEXT("no error"), Original->ErrorCalls.load(), 0);
TestEqual(TEXT("no complete"), Original->CompleteCalls.load(), 0);
TestEqual(TEXT("offline consumed no attempts"), Rig.Fake->NumCalls(), 0);
TestEqual(TEXT("queued count"), Rig.Client.GetQueuedCount(), 1);
TestTrue(TEXT("drained"), Rig.Drain());
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineSecretSnapshotNeverWrittenTest,"VaRest.Http.Offline.SecretSnapshotNeverWritten", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineSecretSnapshotNeverWrittenTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
for (int32 Variant=0;Variant<3;++Variant)
{
    FScopedPersistenceTest Disk;
    FTestRig Rig;
ON_SCOPE_EXIT {Rig.Client.CancelAll(); Rig.Drain();};
    Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
        .WithOfflineQueue(TEXT("guard-boundary"));
    Rig.Connectivity->ReportOffline();
    Rig.Client.SetSecretValue(TEXT("test-secret"),TEXT("s+7"));
    TArray<uint8> Body; Body.Init(120,8192);
    if (Variant==0) { Body.Add(115); Body.Add(43); Body.Add(55); }
    const FString Path=Variant==1 ? TEXT("/write?v=s%2B7") : TEXT("/write");
    const auto Outcome=MakeOutcome<void>();
    const auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
    Observe(Rig.Client.Post<void>(Path).Body(MoveTemp(Body)).Tag(Variant==2 ? FName(TEXT("s+7")) : NAME_None)
        .Idempotent().QueueWhenOffline(),Outcome).OnQueued([Queued](const FString&){ ++*Queued; }).Send();
    Rig.Client.ClearSecretValues();
    if (!TestTrue(TEXT("guard refusal completes"),Rig.Wait(Outcome))) return false;
    TestTrue(TEXT("persistence refused"),Outcome->Error.Kind==EVaRestErrorKind::Config);
    TestEqual(TEXT("no queued acknowledgement"),*Queued,0);
    TestEqual(TEXT("no network"),Rig.Fake->NumCalls(),0);
    if (!TestTrue(TEXT("cleanup drained"),Rig.Drain())) return false;
    const auto Files=MakeShared<TArray<FString>,ESPMode::ThreadSafe>();
    const FString Directory=QueueDirectory(FPaths::Combine(Disk.Root,TEXT("Queue")),TEXT("guard-boundary"));
    LaunchPersistence([Directory]{ return ListFiles(Directory); },[Files](FIoResult&& Result){ *Files=MoveTemp(Result.Names); });
    if (!TestTrue(TEXT("file inspection drained"),Disk.Drain())) return false;
    TestEqual(TEXT("no record, pending or temp written"),Files->Num(),0);
}
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineFlushAdmissionBoundsTest,"VaRest.Http.Offline.FlushAdmissionBounds", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineFlushAdmissionBoundsTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
FTestRig Rig;
ON_SCOPE_EXIT {Rig.Client.CancelAll(); Rig.Drain();};
FScopedPersistenceHooks Hooks(Disk.Root);
Hooks.Hooks->bFailCommit=true;
Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
    .WithOfflineQueue(TEXT("flush-bounds"));
Rig.Connectivity->ReportOffline();
if (!TestTrue(TEXT("load complete"),Rig.Drain())) return false;
const auto Calls=MakeShared<int32,ESPMode::ThreadSafe>(0);
const auto Failed=MakeShared<int32,ESPMode::ThreadSafe>(0);
for (int32 Index=0;Index<4096;++Index)
    if (!TestTrue(TEXT("bounded waiter admitted"),Rig.Client.FlushQueue([Calls,Failed](bool bOk){ ++*Calls; if (!bOk) ++*Failed; }))) return false;
const auto RefusedCallback=MakeShared<int32,ESPMode::ThreadSafe>(0);
TestFalse(TEXT("overflow refused"),Rig.Client.FlushQueue([RefusedCallback](bool){ ++*RefusedCallback; }));
TestEqual(TEXT("accepted callbacks never inline"),*Calls,0);
if (!TestTrue(TEXT("flushes drained"),Rig.Drain())) return false;
TestEqual(TEXT("all accepted succeeded asynchronously"),*Calls,4096);
TestEqual(TEXT("no failure before IO"),*Failed,0);
TestEqual(TEXT("false admission has no callback"),*RefusedCallback,0);
const auto Original=MakeOutcome<void>();
Observe(Rig.Client.Post<void>(TEXT("/save")).Body(TEXT("safe")).Idempotent().QueueWhenOffline(),Original).Send();
if (!TestTrue(TEXT("injected rename failure delivered"),Rig.Wait(Original))) return false;
TestTrue(TEXT("actual error, not queued"),Original->Error.Kind==EVaRestErrorKind::Config);
const auto Io=MakeShared<TOptional<bool>,ESPMode::ThreadSafe>();
if (!TestTrue(TEXT("postfailure flush admitted"),Rig.Client.FlushQueue([Io](bool bOk){ *Io=bOk; }))) return false;
TestFalse(TEXT("failed IO callback not inline"),Io->IsSet());
if (!TestTrue(TEXT("failed IO flush drained"),Rig.Drain())) return false;
if (!TestTrue(TEXT("IO result supplied"),Io->IsSet())) return false;
TestFalse(TEXT("accepted request returns IO failure"),Io->GetValue());
const auto Dropped=MakeShared<int32,ESPMode::ThreadSafe>(0);
if (!TestTrue(TEXT("owner flush admitted"),Rig.Client.FlushQueue([Dropped](bool){ ++*Dropped; }))) return false;
Rig.Client=FVaRestClient();
if (!TestTrue(TEXT("owner flush drained"),Disk.Drain())) return false;
TestEqual(TEXT("expired owner suppresses callback"),*Dropped,0);
return true;
}

namespace VaRest::Http::OfflineTestPrivate
{
    struct FCleanup
    {
        Testing::FTestRig& Rig;
        explicit FCleanup(Testing::FTestRig& InRig):Rig(InRig){}
        ~FCleanup(){ Rig.Client.CancelAll(); Rig.Drain(); }
    };
    void Configure(Testing::FTestRig& Rig,const Testing::FScopedPersistenceTest& Disk,const TCHAR* Name=TEXT("queue"),FVaRestOfflinePolicy Policy={})
    {
        Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue"))).WithOfflineQueue(Name,Policy);
        Rig.Connectivity->ReportOffline();
    }
    class FCustomAuth final : public IVaRestAuth
    {
    public:
        FName Contract,Name=TEXT("CustomQueueTest"); bool bIdentity=true;
        virtual FName GetAuthName() const override { return Name; }
        virtual FName GetIdentityContractName() const override { return Contract; }
        virtual bool GetIdentity(const FVaRestSessionData&,FString& Out) const override { Out=bIdentity ? TEXT("same-account") : TEXT(""); return bIdentity; }
        virtual bool Apply(FVaRestAuthTarget&) const override { return true; }
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineRegistryTest,"VaRest.Http.Offline.RegistryAndReservations",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineRegistryTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::OfflineTestPrivate;
    FScopedPersistenceTest Disk; FTestRig First,Second; FCleanup CleanFirst(First),CleanSecond(Second); FScopedPersistenceHooks Hooks(Disk.Root);
    FVaRestOfflinePolicy Policy; Policy.MaxQueued=1;
    Second.Client.WithConnectivity(First.Connectivity).WithClock(First.Clock);
    Configure(First,Disk,TEXT("shared"),Policy); Configure(Second,Disk,TEXT("shared"),Policy);
    const auto FirstState=FClientAccess::State(First.Client),SecondState=FClientAccess::State(Second.Client);
    TestTrue(TEXT("same exact root/name shares coordinator"),FirstState->Queue==SecondState->Queue);
    if (!TestTrue(TEXT("load drains"),Disk.Drain())) return false;
    Hooks.Hooks->AfterPrepare=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
    const auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0); const auto Rejected=MakeOutcome<void>();
    First.Client.Put<void>(TEXT("/first")).Body(TEXT("a")).QueueWhenOffline().OnQueued([Queued](const FString&){++*Queued;}).Send();
    if (!TestTrue(TEXT("actual prepare barrier"),PumpUntil([&Hooks]{return Hooks.Hooks->AfterPrepare->Entered.load();}))) return false;
    Observe(Second.Client.Put<void>(TEXT("/second")).Body(TEXT("b")).QueueWhenOffline(),Rejected).Send();
    TestEqual(TEXT("reservation visible before commit"),First.Client.GetQueuedCount(),1);
    if (!TestTrue(TEXT("second reservation refused"),Second.Wait(Rejected))) return false;
    TestTrue(TEXT("count bound is Config"),Rejected->Error.Kind==EVaRestErrorKind::Config);
    Hooks.Hooks->AfterPrepare->Released=true;
    if (!TestTrue(TEXT("first accepted"),PumpUntil([Queued]{return *Queued==1;}))) return false;
    Second.Client.WithBaseUrl(TEXT("https://other.test"));
    const auto Conflict=SecondState->GetConfig();
    TestEqual(TEXT("origin conflict detected at registration"),Conflict->QueueConfigError,FString(TEXT("QueueConfig")));
    TestFalse(TEXT("origin conflict cannot become a member"),Conflict->RegisteredQueue.IsValid());
    TestFalse(TEXT("conflicting registration flush refused"),Second.Client.FlushQueue());
    TestEqual(TEXT("old shared item untouched"),First.Client.GetQueuedCount(),1);
    TestEqual(TEXT("conflict no wire"),Second.Fake->NumCalls(),0);
    First.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("first"))); Second.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("conflict")));
    First.Connectivity->ReportOnline(); const auto ConflictRequest=MakeOutcome<void>();
    Observe(Second.Client.Get<void>(TEXT("/conflicting-online")).QueueWhenOffline(),ConflictRequest).Send();
    if (!TestTrue(TEXT("online conflict completes"),Second.Wait(ConflictRequest))) return false;
    TestTrue(TEXT("online conflict fails configuration"),ConflictRequest->Error.Kind==EVaRestErrorKind::Config);
    TestEqual(TEXT("conflicting registration cannot dispatch even online"),Second.Fake->NumCalls(),0);
    TSet<FString> Leaves;
    for (const TCHAR* Name : {TEXT("a/b"),TEXT("a_b"),TEXT("../a"),TEXT("Case"),TEXT("case"),TEXT("юникод")})
    {
        const FString Leaf=FPaths::GetCleanFilename(QueueDirectory(Disk.Root,Name));
        TestEqual(TEXT("hash leaf length"),Leaf.Len(),64); TestFalse(TEXT("no slash"),Leaf.Contains(TEXT("/"))); Leaves.Add(Leaf);
    }
    TestEqual(TEXT("exact names distinct"),Leaves.Num(),6);
    TestTrue(TEXT("drain"),Disk.Drain()); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineBuilderTest,"VaRest.Http.Offline.Builder",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineBuilderTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::OfflineTestPrivate;
    FScopedPersistenceTest Disk; FTestRig Rig; FCleanup Cleanup(Rig);
    auto Bad=MakeOutcome<void>(); Observe(Rig.Client.Get<void>(TEXT("/no-name")).QueueWhenOffline(),Bad).Send();
    TestFalse(TEXT("unnamed refusal noninline"),Bad->IsDone()); if (!TestTrue(TEXT("unnamed completes"),Rig.Wait(Bad))) return false;
    TestTrue(TEXT("unnamed even online Config"),Bad->Error.Kind==EVaRestErrorKind::Config);
    Configure(Rig,Disk);
    for (int32 Variant=0;Variant<4;++Variant)
    {
        auto Outcome=MakeOutcome<void>(); auto Request=Variant==3 ? Rig.Client.Put<void>(TEXT("https://other.test/bad")) : Rig.Client.Post<void>(TEXT("/bad")).Body(TEXT("safe"));
        if (Variant==1) Request=MoveTemp(Request).Idempotent().OnHttpThread();
        if (Variant==2) Request=MoveTemp(Request).Idempotent().SaveTo(FPaths::Combine(Disk.Root,TEXT("out")));
        Observe(MoveTemp(Request).QueueWhenOffline(),Outcome).Send();
        if (!TestTrue(TEXT("invalid option completes"),Rig.Wait(Outcome))) return false;
        TestTrue(TEXT("invalid option Config"),Outcome->Error.Kind==EVaRestErrorKind::Config);
    }
    TestEqual(TEXT("no invalid wire"),Rig.Fake->NumCalls(),0);
    TestEqual(TEXT("no invalid admissions"),Rig.Client.GetQueuedCount(),0);
    TestTrue(TEXT("drained"),Disk.Drain()); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineCommitGateTest,"VaRest.Http.Offline.CommitGate",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineCommitGateTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::OfflineTestPrivate;
    for (bool bAfterCutoff : {false,true})
    {
        FScopedPersistenceTest Disk; FTestRig Rig; FCleanup Cleanup(Rig); FScopedPersistenceHooks Hooks(Disk.Root);
        Configure(Rig,Disk); if (!TestTrue(TEXT("loaded"),Disk.Drain())) return false;
        const auto Barrier=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
        if (bAfterCutoff) Hooks.Hooks->BeforeRename=Barrier; else Hooks.Hooks->AfterPrepare=Barrier;
        const auto Outcome=MakeOutcome<void>(); const auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
        const auto Probe=MakeShared<FOutcomeProbe,ESPMode::ThreadSafe>(); Rig.Client.Use(Probe,EVaRestStageSlot::Outer);
        const auto Handle=Observe(Rig.Client.Put<void>(TEXT("/cutoff")).Body(TEXT("safe")).QueueWhenOffline(),Outcome).OnQueued([Queued](const FString&){++*Queued;}).Send();
        if (!TestTrue(TEXT("real worker held"),PumpUntil([Barrier]{return Barrier->Entered.load();}))) return false;
        TestEqual(TEXT("no pre-rename acknowledgement"),*Queued,0);
        TestEqual(TEXT("reserved during worker hold"),Rig.Client.GetQueuedCount(),1);
        TestEqual(TEXT("active tracks commit selection"),Handle.IsActive(),!bAfterCutoff);
        Handle.Cancel();
        if (!bAfterCutoff)
        {
            if (!TestTrue(TEXT("prompt canceled original"),Rig.Wait(Outcome))) return false;
            TestTrue(TEXT("canceled error"),Outcome->Error.Kind==EVaRestErrorKind::Canceled);
            TestEqual(TEXT("one chain outcome before release"),Probe->Total(Handle.GetId()),1);
        }
        else TestFalse(TEXT("commit callback still pending"),Outcome->IsDone());
        Barrier->Released=true;
        if (!TestTrue(TEXT("worker and callback drain"),Rig.Drain())) return false;
        TestEqual(TEXT("exact accepted acknowledgement"),*Queued,bAfterCutoff?1:0);
        TestEqual(TEXT("exact surviving item"),Rig.Client.GetQueuedCount(),bAfterCutoff?1:0);
        TestEqual(TEXT("one outer outcome"),Probe->Total(Handle.GetId()),1);
        TestFalse(TEXT("handle ended"),Handle.IsActive());
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineFlushCutoffTest,"VaRest.Http.Offline.FlushAdmissionCutoff",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineFlushCutoffTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::OfflineTestPrivate;
    FScopedPersistenceTest Disk; FTestRig Rig; FCleanup Cleanup(Rig); FScopedPersistenceHooks Hooks(Disk.Root);
    Configure(Rig,Disk); if (!TestTrue(TEXT("loaded"),Disk.Drain())) return false;
    const auto Barrier=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->AfterPrepare=Barrier;
    const auto Order=MakeShared<TArray<int32>,ESPMode::ThreadSafe>();
    Rig.Client.Put<void>(TEXT("/flush")).Body(TEXT("safe")).QueueWhenOffline().OnQueued([Order](const FString&){Order->Add(1);}).Send();
    TestTrue(TEXT("flush accepted"),Rig.Client.FlushQueue([Order](bool bOk){Order->Add(bOk?2:9);}));
    if (!TestTrue(TEXT("prepare held"),PumpUntil([Barrier]{return Barrier->Entered.load();}))) return false;
    TestTrue(TEXT("neither ack nor flush before prepared handoff"),Order->IsEmpty());
    Barrier->Released=true;
    if (!TestTrue(TEXT("flush completed"),PumpUntil([Order]{return Order->Num()==2;}))) return false;
    TestTrue(TEXT("logical watermark waits for rename"),*Order==TArray<int32>{1,2});
    TestTrue(TEXT("drains"),Disk.Drain()); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineLoadDrainTest,"VaRest.Http.Offline.LoadDrainFifo",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineLoadDrainTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::OfflineTestPrivate;
    FScopedPersistenceTest Disk; FTestRig Rig; FCleanup Cleanup(Rig); FScopedPersistenceHooks Hooks(Disk.Root);
    const auto Barrier=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->BeforeQueueLoad=Barrier;
    Configure(Rig,Disk);
    struct FSeen { TArray<int32> Order,Counts; int32 Errors=0; bool bDuringDrain=false; };
    const auto Seen=MakeShared<FSeen,ESPMode::ThreadSafe>(); auto Client=Rig.Client;
    const auto A=MakeOutcome<void>();
    // This shape passes Resolve and pre-load body admission, then fails Start's metadata gate.
    auto Request=Rig.Client.Put<void>(TEXT("/A")).Body(TEXT("a")).QueueWhenOffline();
    for (int32 I=0;I<129;++I) Request=MoveTemp(Request).Header(FString::Printf(TEXT("X-H%d"),I),TEXT("v"));
    MoveTemp(Request).OnError([Seen,Client](const FVaRestError& Error) mutable
    {
        ++Seen->Errors; Seen->bDuringDrain=Error.Kind==EVaRestErrorKind::Config;
        Seen->Counts.Add(Client.GetQueuedCount());
        Client.Put<void>(TEXT("/C")).Body(TEXT("c")).QueueWhenOffline().OnQueued([Seen](const FString&){Seen->Order.Add(3);}).Send();
        Seen->Counts.Add(Client.GetQueuedCount());
    }).Send();
    Rig.Client.Put<void>(TEXT("/B")).Body(TEXT("b")).QueueWhenOffline().OnQueued([Seen](const FString&){Seen->Order.Add(2);}).Send();
    if (!TestTrue(TEXT("load held"),PumpUntil([Barrier]{return Barrier->Entered.load();}))) return false;
    TestEqual(TEXT("both waiters visible"),Rig.Client.GetQueuedCount(),2);
    Barrier->Released=true;
    if (!TestTrue(TEXT("default budget admits after load releases"),PumpUntil([Seen]{return Seen->Order.Num()==2;}))) return false;
    TestEqual(TEXT("A rejected during drain once"),Seen->Errors,1); TestTrue(TEXT("A reached Start Config"),Seen->bDuringDrain);
    TestTrue(TEXT("older B counted through reentrancy"),Seen->Counts==TArray<int32>{1,2});
    TestTrue(TEXT("B committed before reentrant C"),Seen->Order==TArray<int32>{2,3});
    TestEqual(TEXT("B+C durable"),Rig.Client.GetQueuedCount(),2);
    TestTrue(TEXT("drains"),Disk.Drain()); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineLoadHandoffRefusalTest,"VaRest.Http.Offline.LoadHandoffRefusal",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineLoadHandoffRefusalTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::OfflineTestPrivate;
    FScopedPersistenceTest Disk; FTestRig Rig; FCleanup Cleanup(Rig); FScopedPersistenceHooks Hooks(Disk.Root);
    const auto Barrier=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->BeforeQueueLoad=Barrier;
    Configure(Rig,Disk); auto Outcome=MakeOutcome<void>();
    Observe(Rig.Client.Put<void>(TEXT("/waiting")).Body(TEXT("safe")).QueueWhenOffline(),Outcome).Send();
    if (!TestTrue(TEXT("load held"),PumpUntil([Barrier]{return Barrier->Entered.load();}))) return false;
    const auto Release=MakeShared<bool,ESPMode::ThreadSafe>(false);
    ON_SCOPE_EXIT { *Release=true; Barrier->Released=true; Rig.Drain(); };
    for (int32 I=0;I<8192;++I) if (!TestTrue(TEXT("occupied owned continuation"),PostPersistencePoll([Release]{return *Release;}))) return false;
    const uint64 Launched=PersistenceCounters().Launched.load(); Barrier->Released=true;
    if (!TestTrue(TEXT("handoff refusal completes original"),Rig.Wait(Outcome))) return false;
    TestTrue(TEXT("handoff refusal Config"),Outcome->Error.Kind==EVaRestErrorKind::Config);
    TestEqual(TEXT("no Prepare IO admitted"),PersistenceCounters().Launched.load(),Launched);
    TestEqual(TEXT("no stranded waiter/count"),Rig.Client.GetQueuedCount(),0);
    TestFalse(TEXT("still occupied at callback"),PostPersistenceContinuation([]{}));
    *Release=true; if (!TestTrue(TEXT("all occupied slots drain"),Rig.Drain())) return false;
    const auto Flushed=MakeShared<TOptional<bool>,ESPMode::ThreadSafe>();
    TestTrue(TEXT("flush observation admitted"),Rig.Client.FlushQueue([Flushed](bool bOk){*Flushed=bOk;}));
    if (!TestTrue(TEXT("flush drains"),Rig.Drain())) return false;
    TestFalse(TEXT("unavailable storage never flushes successfully"),Flushed->Get(true)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineRegistrationGapTest,"VaRest.Http.Offline.RegistrationGap",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineRegistrationGapTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::OfflineTestPrivate;
    for (bool bRoot : {false,true})
    {
        FScopedPersistenceTest Disk; FTestRig Rig; FCleanup Cleanup(Rig); Configure(Rig,Disk,TEXT("old"));
        if (!TestTrue(TEXT("old loaded"),Disk.Drain())) return false;
        const auto Old=FClientAccess::State(Rig.Client)->Queue; const auto Outcome=MakeOutcome<void>();
        struct FGap { int32 Count=-1; bool bUnavailable=false,bFlush=true; }; const auto Gap=MakeShared<FGap,ESPMode::ThreadSafe>();
        auto Client=Rig.Client;
        QueueRegistrationGapForTest()=[Client,Gap,Outcome]() mutable
        {
            const auto Current=FClientAccess::State(Client)->GetConfig();
            Gap->bUnavailable=!Current->RegisteredQueue.IsValid() && !Current->QueueConfigError.IsEmpty();
            Gap->Count=Client.GetQueuedCount(); Gap->bFlush=Client.FlushQueue();
            Observe(Client.Put<void>(TEXT("/gap")).Body(TEXT("safe")).QueueWhenOffline(),Outcome).Send();
        };
        ON_SCOPE_EXIT { QueueRegistrationGapForTest().Reset(); };
        if (bRoot) Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("NewQueue")));
        else Rig.Client.WithOfflineQueue(TEXT("new"));
        QueueRegistrationGapForTest().Reset();
        if (!TestTrue(TEXT("gap request completes"),Rig.Wait(Outcome))) return false;
        TestTrue(TEXT("coherent unavailable publication"),Gap->bUnavailable); TestFalse(TEXT("gap flush refused"),Gap->bFlush);
        TestEqual(TEXT("gap count cannot expose old queue"),Gap->Count,0); TestTrue(TEXT("gap Config"),Outcome->Error.Kind==EVaRestErrorKind::Config);
        TestEqual(TEXT("old queue unchanged"),Old->Count(),0);
        const auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
        Rig.Client.Put<void>(TEXT("/new")).Body(TEXT("safe")).QueueWhenOffline().OnQueued([Queued](const FString&){++*Queued;}).Send();
        if (!TestTrue(TEXT("complete new snapshot admits"),PumpUntil([Queued]{return *Queued==1;}))) return false;
        TestEqual(TEXT("new queue count"),Rig.Client.GetQueuedCount(),1); TestEqual(TEXT("no old-domain file"),Old->Count(),0);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineAuthContractsTest,"VaRest.Http.Offline.AuthContracts",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineAuthContractsTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::OfflineTestPrivate;
    FScopedPersistenceTest Disk; FTestRig A,B; FCleanup CleanA(A),CleanB(B);
    B.Client.WithConnectivity(A.Connectivity).WithClock(A.Clock);
    A.Client.WithAuth(FVaRestAuth::Bearer()); B.Client.WithAuth(FVaRestAuth::Bearer());
    Configure(A,Disk,TEXT("builtin")); Configure(B,Disk,TEXT("builtin"));
    TestTrue(TEXT("builtin contract before credentials"),FClientAccess::State(A.Client)->Queue==FClientAccess::State(B.Client)->Queue);
    if (!TestTrue(TEXT("builtin load drains"),Disk.Drain())) return false;
    const auto First=MakeShared<FCustomAuth,ESPMode::ThreadSafe>(); const auto Other=MakeShared<FCustomAuth,ESPMode::ThreadSafe>();
    A.Client.WithOfflineQueue(TEXT("")).WithAuth(First).WithOfflineQueue(TEXT("custom"));
    B.Client.WithOfflineQueue(TEXT("")).WithAuth(First).WithOfflineQueue(TEXT("custom"));
    TestTrue(TEXT("same unlabelled instance shares"),FClientAccess::State(A.Client)->Queue==FClientAccess::State(B.Client)->Queue);
    B.Client.WithAuth(Other);
    TestFalse(TEXT("different unlabelled same-name instance refuses"),FClientAccess::State(B.Client)->GetConfig()->RegisteredQueue.IsValid());
    if (!TestTrue(TEXT("custom load drains"),Disk.Drain())) return false;
    A.Client.WithOfflineQueue(TEXT("")); B.Client.WithOfflineQueue(TEXT(""));
    First->Contract=TEXT("Compatible"); Other->Contract=TEXT("compatible"); Other->Name=TEXT("customqueuetest");
    A.Client.WithOfflineQueue(TEXT("declared")); B.Client.WithOfflineQueue(TEXT("declared"));
    TestTrue(TEXT("declared cross-instance compatibility"),FClientAccess::State(A.Client)->Queue==FClientAccess::State(B.Client)->Queue);
    if (!TestTrue(TEXT("declared load drains"),Disk.Drain())) return false;
    Other->bIdentity=false;
    auto Outcome=MakeOutcome<void>(); Observe(B.Client.Put<void>(TEXT("/unsupported")).QueueWhenOffline(),Outcome).Send();
    if (!TestTrue(TEXT("real identity refusal completes"),B.Wait(Outcome))) return false;
    TestTrue(TEXT("declaration does not supply identity"),Outcome->Error.Kind==EVaRestErrorKind::Config);
    return true;
}

namespace VaRest::Http::OfflineTestPrivate
{
    class FAnswer final : public IVaRestStage
    {
    public:
        TSharedPtr<FVaRestOutcomeCallback,ESPMode::ThreadSafe> Saved;
        virtual FName GetStageName() const override { return TEXT("OfflineOuterAnswer"); }
        virtual void Process(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done) override
        {
            Saved=MakeShared<FVaRestOutcomeCallback,ESPMode::ThreadSafe>(MoveTemp(Done));
            Next.Proceed(Exchange,[Slot=Saved](FVaRestAttemptOutcome&& R) mutable {if (*Slot) {auto Callback=MoveTemp(*Slot);Callback(MoveTemp(R));}});
        }
        void Answer(){if (Saved.IsValid() && *Saved) {auto Callback=MoveTemp(*Saved); FVaRestAttemptOutcome R; R.Status=200; Callback(MoveTemp(R));}}
    };
    class FRefreshFailure final : public IVaRestAuth
    {
    public:
        virtual FName GetAuthName() const override { return TEXT("QueueRefreshFailure"); }
        virtual bool GetIdentity(const FVaRestSessionData&,FString& Out) const override {Out=TEXT("account");return true;}
        virtual bool Apply(FVaRestAuthTarget& Target) const override {Target.Headers.Set(TEXT("Authorization"),TEXT("Bearer ")+Target.Session.AccessToken);return true;}
        virtual bool CanRefresh(const FVaRestSessionData&) const override {return true;}
        virtual void Refresh(const FVaRestAuthRefreshContext&,TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done) override
        {Done(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Connection,TEXT("test network failure")));}
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineRetryBodyTest,"VaRest.Http.Offline.RetryBody",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineRetryBodyTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::OfflineTestPrivate;
    for (int32 Mode=0;Mode<3;++Mode)
    {
        FScopedPersistenceTest Disk; FTestRig Rig; FCleanup Cleanup(Rig);
        if (Mode==2) {Rig.Client.WithAuth(MakeShared<FRefreshFailure,ESPMode::ThreadSafe>());Rig.Client.SetBearerToken(TEXT("token"));}
        Configure(Rig,Disk); if (!TestTrue(TEXT("loaded"),Disk.Drain())) return false; Rig.Connectivity->ReportOnline();
        const auto Q=MakeShared<int32,ESPMode::ThreadSafe>(0); const auto Original=MakeOutcome<void>(); FScopedTraceRecorder Trace;
        auto Policy=FVaRestRetryPolicy::Exponential(2); Policy.Backoff.BaseDelay=0; Policy.Backoff.MaxDelay=0;
        if (Mode==2) Rig.Fake->Enqueue(FVaRestFakeResponse::Text(401,TEXT("unauthorized")));
        else for (int32 I=0;I<(Mode==1?3:1);++I) Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection));
        const TArray<uint8> Body={0,1,255,19,7};
        const auto Handle=Observe(Rig.Client.Put<void>(TEXT("/payload")).Body(Body).Retry(Mode==1?Policy:FVaRestRetryPolicy::Disabled()).QueueWhenOffline(),Original)
            .OnQueued([Q](const FString&){++*Q;}).Send();
        if (Mode==1) for (int32 Retry=1;Retry<=2;++Retry)
        {
            if (!TestTrue(TEXT("request retry scheduled"),PumpUntil([&Trace,Handle,Retry]
            {
                int32 Count=0; for (const auto& E:Trace.Recorder->For(Handle.GetId()))
                    if (E.Kind==EVaRestTraceKind::Note && E.Payload.Get<FVaRestTraceNote>().Kind==EVaRestTraceNoteKind::RetryScheduled) ++Count;
                return Count>=Retry;
            }))) return false;
            Rig.Clock->Advance(0);
        }
        if (!TestTrue(*FString::Printf(TEXT("final connection failure queues mode %d"),Mode),PumpUntil([Q]{return *Q==1;}))) return false;
        TestEqual(TEXT("all retries precede durable admission"),Rig.Fake->NumCalls(),Mode==1?3:1);
        TestEqual(TEXT("no typed callback on durable acceptance"),Original->CompleteCalls.load(),0);
        // Stay offline after admission so a normal scheduled replay cannot consume the recovery fixture.
        Rig.Connectivity->ReportOffline();
        const auto Loaded=MakeShared<FQueueLoad,ESPMode::ThreadSafe>();
        FQueueStore::Load(QueueDirectory(FPaths::Combine(Disk.Root,TEXT("Queue")),TEXT("queue")),TEXT("https://api.test"),{},[Loaded](FQueueLoad&& R){*Loaded=MoveTemp(R);});
        if (!TestTrue(TEXT("inspect durable payload"),Disk.Drain()) || !TestEqual(TEXT("one item"),Loaded->Items.Num(),1)) return false;
        TestTrue(TEXT("failed wire did not consume durable bytes"),Loaded->Items[0].Body.Bytes==Body);
        for (int32 I=0;I<Rig.Fake->NumCalls();++I) TestTrue(TEXT("every physical retry same payload"),Rig.Fake->GetCall(I).Request.Body==Body);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineEarlyTerminalTest,"VaRest.Http.Offline.EarlyTerminal",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineEarlyTerminalTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::OfflineTestPrivate;
    for (int32 Mode=0;Mode<4;++Mode)
    {
        FScopedPersistenceTest Disk; FTestRig Rig; FCleanup Cleanup(Rig); FScopedPersistenceHooks Hooks(Disk.Root);
        Configure(Rig,Disk); if (!TestTrue(TEXT("loaded"),Disk.Drain())) return false;
        const auto Barrier=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->AfterPrepare=Barrier;
        ON_SCOPE_EXIT {Barrier->Released=true; Rig.Drain(); FClientState::ResetShutdownInternal();};
        const auto Outer=MakeShared<FAnswer,ESPMode::ThreadSafe>(); if (Mode==2) Rig.Client.Use(Outer,EVaRestStageSlot::Outer);
        const auto Q=MakeShared<int32,ESPMode::ThreadSafe>(0); const auto Outcome=MakeOutcome<void>();
        auto Request=Rig.Client.Put<void>(TEXT("/early")).Body(TEXT("safe")).QueueWhenOffline();
        UObject* Owner=nullptr;
        if (Mode==0) Request=MoveTemp(Request).Retry(FVaRestRetryPolicy::Disabled().WithDeadline(1));
        if (Mode==1) {Owner=NewObject<UVaRestHttpSettings>(GetTransientPackage(),NAME_None,RF_Transient);Request=MoveTemp(Request).Owner(Owner);}
        const auto H=Observe(MoveTemp(Request),Outcome).OnQueued([Q](const FString&){++*Q;}).Send();
        if (!TestTrue(TEXT("prepared bytes held"),PumpUntil([Barrier]{return Barrier->Entered.load();}))) return false;
        if (Mode==0) Rig.Clock->Advance(1);
        if (Mode==1) Owner->MarkAsGarbage();
        if (Mode==2) Outer->Answer();
        if (Mode==3) FClientState::CancelForShutdown();
        if (Mode==0 || Mode==2) if (!TestTrue(TEXT("early typed completion before worker release"),Rig.Wait(Outcome))) return false;
        Barrier->Released=true;
        if (!TestTrue(TEXT("canceled worker drains"),Rig.Drain())) return false;
        TestFalse(TEXT("handle ended"),H.IsActive()); TestEqual(TEXT("never acknowledges abandoned work"),*Q,0);
        TestEqual(TEXT("no residual reservation"),Rig.Client.GetQueuedCount(),0);
        if (Mode==0) TestTrue(TEXT("deadline error"),Outcome->Error.Kind==EVaRestErrorKind::Timeout);
        if (Mode==1 || Mode==3) TestEqual(TEXT("owner and shutdown silent"),Outcome->CompleteCalls.load(),0);
        if (Mode==2) TestEqual(TEXT("outer answer only once"),Outcome->CompleteCalls.load(),1);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineRootBoundsTest,"VaRest.Http.Offline.RootBounds",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineRootBoundsTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::OfflineTestPrivate;
    FScopedPersistenceTest Disk; FTestRig A,B; FCleanup CleanA(A),CleanB(B);
    B.Client.WithConnectivity(A.Connectivity).WithClock(A.Clock); A.Connectivity->ReportOffline();
    A.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Case"))).WithOfflineQueue(TEXT("same"));
    B.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("case"))).WithOfflineQueue(TEXT("same"));
#if PLATFORM_LINUX
    TestTrue(TEXT("Linux roots preserve native case"),FClientAccess::State(A.Client)->Queue!=FClientAccess::State(B.Client)->Queue);

#endif
    if (!TestTrue(TEXT("roots loaded"),Disk.Drain())) return false;
    const uint64 Before=PersistenceCounters().Launched.load();
    B.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,FString::ChrN(4097,TEXT('x'))));
    TestFalse(TEXT("oversized generated path unavailable"),FClientAccess::State(B.Client)->GetConfig()->RegisteredQueue.IsValid());
    TestEqual(TEXT("oversized root performs no IO"),PersistenceCounters().Launched.load(),Before);
    TestEqual(TEXT("valid peer still independent"),A.Client.GetQueuedCount(),0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineMultipartTest,"VaRest.Http.Offline.MultipartGuard",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineMultipartTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::OfflineTestPrivate;
    for (int32 Mode=0;Mode<4;++Mode)
    {
        FScopedPersistenceTest Disk; FTestRig Rig; FCleanup Cleanup(Rig); Configure(Rig,Disk);
        if (!TestTrue(TEXT("loaded"),Disk.Drain())) return false;
        FVaRestMultipart Parts(TEXT("BoundaryQueueTest"));
        if (Mode==3)
        {
            const FString File=FPaths::Combine(Disk.Root,TEXT("body.bin")); const auto Written=MakeShared<bool,ESPMode::ThreadSafe>(false);
            LaunchPersistence([File]{return WriteNewFile(File,TArray<uint8>{1,2,3});},[Written](FIoResult&& R){*Written=R.bOk;});
            if (!TestTrue(TEXT("file fixture created"),Disk.Drain()) || !TestTrue(TEXT("file write"),*Written)) return false;
            Parts.AddFile(TEXT("file"),File);
        }
        else
        {
            for (int32 I=0;I<40;++I) Parts.AddField(FString::Printf(TEXT("field%d"),I),TEXT("safe"));
            Parts.AddField(Mode==1?TEXT("password"):TEXT("last"),Mode==2?FString::ChrN(8192,TEXT('x'))+TEXT("s+7"):TEXT("safe"));
            if (Mode==2) Rig.Client.SetSecretValue(TEXT("tail-secret"),TEXT("s+7"));
        }
        const auto Outcome=MakeOutcome<void>(); const auto Q=MakeShared<int32,ESPMode::ThreadSafe>(0);
        Observe(Rig.Client.Post<void>(TEXT("/parts")).Body(Parts).Idempotent().QueueWhenOffline(),Outcome).OnQueued([Q](const FString&){++*Q;}).Send();
        if (Mode==0) {if (!TestTrue(TEXT("safe memory multipart accepted"),PumpUntil([Q]{return *Q==1;}))) return false;}
        else
        {
            if (!TestTrue(TEXT("unsafe multipart refused"),Rig.Wait(Outcome))) return false;
            TestTrue(TEXT("descriptor, full body and stream gates Config"),Outcome->Error.Kind==EVaRestErrorKind::Config);
            TestEqual(TEXT("no unsafe ack"),*Q,0); TestEqual(TEXT("no unsafe reservation"),Rig.Client.GetQueuedCount(),0);
            const auto Files=MakeShared<TArray<FString>,ESPMode::ThreadSafe>();
            const FString Directory=QueueDirectory(FPaths::Combine(Disk.Root,TEXT("Queue")),TEXT("queue"));
            LaunchPersistence([Directory]{return ListFiles(Directory);},[Files](FIoResult&& R){*Files=MoveTemp(R.Names);});
            if (!TestTrue(TEXT("file absence observed"),Disk.Drain())) return false; TestTrue(TEXT("no unsafe record"),Files->IsEmpty());
        }
        TestEqual(TEXT("no multipart wire while offline"),Rig.Fake->NumCalls(),0);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineCallbackLifetimeTest,"VaRest.Http.Offline.CallbackLifetime",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineCallbackLifetimeTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::OfflineTestPrivate;
    FScopedPersistenceTest Disk; FTestRig Rig; Configure(Rig,Disk);
    auto Owner=MakeShared<FVaRestClient,ESPMode::ThreadSafe>(Rig.Client); const TWeakPtr<FClientState,ESPMode::ThreadSafe> Weak=FClientAccess::State(*Owner);
    Rig.Client=FVaRestClient(); const auto Q=MakeShared<int32,ESPMode::ThreadSafe>(0);
    Owner->Put<void>(TEXT("/destroy")).Body(TEXT("safe")).QueueWhenOffline().OnQueued([Owner,Q](const FString&){++*Q;*Owner=FVaRestClient();}).Send();
    if (!TestTrue(TEXT("callback destroys last public handle"),PumpUntil([Q]{return *Q==1;}))) return false;
    if (!TestTrue(TEXT("owned work releases"),Disk.Drain())) return false;
    TestFalse(TEXT("state not retained by callback/coordinator cycle"),Weak.IsValid());
    return true;
}

namespace VaRest::Http::OfflineTestPrivate
{
    class FRefreshOutsideStorage final : public IVaRestAuth
    {
    public:
        explicit FRefreshOutsideStorage(TSharedRef<FVaRestConnectivity,ESPMode::ThreadSafe> InReachability):Reachability(InReachability){}
        TSharedRef<FVaRestConnectivity,ESPMode::ThreadSafe> Reachability;
        TSharedRef<TOptional<EVaRestErrorKind>,ESPMode::ThreadSafe> Finished=MakeShared<TOptional<EVaRestErrorKind>,ESPMode::ThreadSafe>();
        virtual FName GetAuthName() const override {return TEXT("RefreshOutsideStorage");}
        virtual bool GetIdentity(const FVaRestSessionData&,FString& Out) const override {Out=TEXT("stable");return true;}
        virtual bool Apply(FVaRestAuthTarget& Target) const override {Target.Headers.Set(TEXT("Authorization"),TEXT("Bearer ")+Target.Session.AccessToken);return true;}
        virtual bool CanRefresh(const FVaRestSessionData&) const override {return true;}
        virtual void Refresh(const FVaRestAuthRefreshContext& Context,TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done) override
        {
            Reachability->ReportOffline();
            const auto Callback=MakeShared<TUniqueFunction<void(FVaRestAuthRefreshResult&&)>,ESPMode::ThreadSafe>(MoveTemp(Done));
            Context.Request<void>(TEXT("POST"),TEXT("/refresh")).OnError([Callback,Seen=Finished](const FVaRestError& Error)
            {*Seen=Error.Kind;(*Callback)(FVaRestAuthRefreshResult::Failed(Error.Kind,Error.Message));}).Send();
        }
    };
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineRefreshBypassTest,"VaRest.Http.Offline.RefreshBypassesStorage",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineRefreshBypassTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::OfflineTestPrivate;
    FScopedPersistenceTest Disk; FTestRig Rig; FCleanup Cleanup(Rig); FScopedPersistenceHooks Hooks(Disk.Root);
    const auto Auth=MakeShared<FRefreshOutsideStorage,ESPMode::ThreadSafe>(Rig.Connectivity); Rig.Client.WithAuth(Auth); Rig.Client.SetBearerToken(TEXT("old-token"));
    Configure(Rig,Disk); if (!TestTrue(TEXT("loaded"),Disk.Drain())) return false;
    const auto Barrier=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->AfterPrepare=Barrier;
    const auto Q=MakeShared<int32,ESPMode::ThreadSafe>(0);
    Rig.Client.Put<void>(TEXT("/first")).Body(TEXT("safe")).QueueWhenOffline().OnQueued([Q](const FString&){++*Q;}).Send();
    if (!TestTrue(TEXT("durable worker held"),PumpUntil([Barrier]{return Barrier->Entered.load();}))) return false;
    Rig.Connectivity->ReportOnline(); Rig.Fake->Enqueue(FVaRestFakeResponse::Text(401,TEXT("unauthorized")));
    Rig.Client.Put<void>(TEXT("/second")).Body(TEXT("safe")).QueueWhenOffline().OnQueued([Q](const FString&){++*Q;}).Send();
    if (!TestTrue(TEXT("real refresh request completes while storage held"),PumpUntil([Auth]{return Auth->Finished->IsSet();}))) return false;
    TestTrue(TEXT("refresh forced offline truthful kind"),Auth->Finished->GetValue()==EVaRestErrorKind::Offline);
    TestEqual(TEXT("refresh never adds wire while offline"),Rig.Fake->NumCalls(),1); TestEqual(TEXT("no premature durable ack"),*Q,0);
    Barrier->Released=true; if (!TestTrue(TEXT("both originals durable after release"),PumpUntil([Q]{return *Q==2;}))) return false;
    TestEqual(TEXT("refresh did not create third queue record"),Rig.Client.GetQueuedCount(),2); return true;
}

#endif
