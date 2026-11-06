// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS
#include "VaRestHttpTesting.h"
#include "VaRestQueueStore.h"
#include "VaRestClientState.h"
#include "VaRestIdentity.h"
#include "VaRestPersistence.h"
#include "VaRestAuth.h"
#include "VaRestDiskCache.h"
#include "VaRestExchangeState.h"
#include "VaRestHttpSettings.h"
#include "UObject/Package.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#include "Tasks/Task.h"
#if PLATFORM_LINUX
#include <sys/stat.h>
#endif

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
for (int32 Variant=0;Variant<4;++Variant)
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
        .Endpoint(Variant==3 ? FName(TEXT("s+7")) : NAME_None)
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
    TestTrue(TEXT("held Prepare metadata is actually reserved"),FirstState->Queue->MetadataBytesForTest()>0);
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

#if PLATFORM_LINUX
namespace VaRest::Http::OfflineTestPrivate
{
    // One owned, joined test task; permission work cannot sit behind the held serial IO pipe.
    bool FileTask(TUniqueFunction<void()>&& Work)
    {
        auto Task=UE::Tasks::Launch(TEXT("VaRest.QueuePermissionTest"),[Job=MoveTemp(Work)]() mutable {check(!IsInGameThread());Job();});
        return Testing::PumpUntil([&Task]{return Task.IsCompleted();});
    }
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineCleanupFailureTest,"VaRest.Http.Offline.CleanupFailure",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineCleanupFailureTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::OfflineTestPrivate;
    // Successful abort, true-missing abort, denied abort, denied commit cleanup, known-clean failed commit.
    for (int32 Mode=0;Mode<5;++Mode)
    {
        AddInfo(FString::Printf(TEXT("cleanup mode %d"),Mode));
        FScopedPersistenceTest Disk; FTestRig Rig; FCleanup Cleanup(Rig); FScopedPersistenceHooks Hooks(Disk.Root);
        FVaRestOfflinePolicy Policy; Policy.MaxQueued=1; Configure(Rig,Disk,TEXT("cleanup"),Policy);
        if (!TestTrue(TEXT("loaded"),Disk.Drain())) return false;
        const FString Directory=FClientAccess::State(Rig.Client)->Queue->GetDirectory();
        const auto Barrier=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
        if (Mode<3) Hooks.Hooks->AfterPrepare=Barrier; else Hooks.Hooks->BeforeRename=Barrier;
        Hooks.Hooks->bFailCommit=Mode==4;
        ON_SCOPE_EXIT {FileTask([Directory]{chmod(TCHAR_TO_UTF8(*PersistencePathForIo(Directory)),0700);});Barrier->Released=true;Rig.Drain();};
        const auto Outcome=MakeOutcome<void>(); const auto Ack=MakeShared<int32,ESPMode::ThreadSafe>(0);
        const auto H=Observe(Rig.Client.Put<void>(TEXT("/cleanup")).Body(TEXT("safe")).QueueWhenOffline(),Outcome).OnQueued([Ack](const FString&){++*Ack;}).Send();
        if (!TestTrue(TEXT("real pending held"),PumpUntil([Barrier]{return Barrier->Entered.load();}))) return false;
        const auto Pending=MakeShared<FString,ESPMode::ThreadSafe>(); const auto Witness=MakeShared<bool,ESPMode::ThreadSafe>(false);
        FileTask([Directory,Pending,Witness,Mode]
        {
            const auto Files=ListFiles(Directory); if (!Files.bOk || Files.Names.Num()!=1 || !Files.Names[0].EndsWith(TEXT(".pending"))) return;
            *Pending=FPaths::Combine(Directory,Files.Names[0]);
            if (Mode==1) {*Witness=DeleteFile(*Pending).bOk && InspectPersistencePath(*Pending).Kind==EPathKind::Missing;return;}
            if (Mode==2 || Mode==3)
            {
                *Witness=chmod(TCHAR_TO_UTF8(*PersistencePathForIo(Directory)),0500)==0
                    && !DeleteFile(*Pending).bOk && InspectPersistencePath(*Pending).Kind==EPathKind::Regular;
            }
            else *Witness=InspectPersistencePath(*Pending).Kind==EPathKind::Regular;
        });
        if (!TestTrue(TEXT("actual physical pending/denied-unlink witness"),*Witness)) return false;
        if (Mode<3)
        {
            H.Cancel(); if (!TestTrue(TEXT("logical cancellation prompt before IO release"),Rig.Wait(Outcome))) return false;
            TestTrue(TEXT("one canceled outcome"),Outcome->Error.Kind==EVaRestErrorKind::Canceled);
        }
        const auto Flushed=MakeShared<TOptional<bool>,ESPMode::ThreadSafe>();
        TestTrue(TEXT("flush admitted while cleanup pending"),Rig.Client.FlushQueue([Flushed](bool bOk){*Flushed=bOk;}));
        TestFalse(TEXT("flush not inline"),Flushed->IsSet());
        Barrier->Released=true; if (!TestTrue(TEXT("physical cleanup drains"),Rig.Drain())) return false;
        if (!TestTrue(TEXT("flush result delivered"),Flushed->IsSet())) return false;
        TestEqual(TEXT("cleanup failure reaches accepted Flush"),Flushed->GetValue(),Mode<2);
        TestEqual(TEXT("no acknowledgement"),*Ack,0);TestEqual(TEXT("one original result"),Outcome->CompleteCalls.load(),1);
        TestEqual(TEXT("resolved physical reservation"),Rig.Client.GetQueuedCount(),0);
        FileTask([Directory,Pending,Witness,Mode]
        {
            const auto Files=ListFiles(Directory);*Witness=Files.bOk;
            for (const FString& Name:Files.Names) if (Name.EndsWith(TEXT(".json"))) *Witness=false;
            *Witness=*Witness && (InspectPersistencePath(*Pending).Kind==((Mode==2 || Mode==3)?EPathKind::Regular:EPathKind::Missing));
            chmod(TCHAR_TO_UTF8(*PersistencePathForIo(Directory)),0700);
        });
        TestTrue(TEXT("no replayable file; failed cleanup remains physically present"),*Witness);
        const uint64 Before=PersistenceCounters().Launched.load();
        const auto Next=MakeOutcome<void>(); const auto NextAck=MakeShared<int32,ESPMode::ThreadSafe>(0);
        Hooks.Hooks->bFailCommit=false;
        Observe(Rig.Client.Put<void>(TEXT("/next")).Body(TEXT("safe")).QueueWhenOffline(),Next).OnQueued([NextAck](const FString&){++*NextAck;}).Send();
        if (!TestTrue(TEXT("next operation resolves"),Rig.Drain())) return false;
        if (Mode==2 || Mode==3)
        {
            TestTrue(TEXT("cleanup error makes storage unavailable"),Next->Error.Kind==EVaRestErrorKind::Config);
            TestEqual(TEXT("unavailable queue launches no new IO"),PersistenceCounters().Launched.load(),Before);
            TestEqual(TEXT("unavailable queue cannot acknowledge"),*NextAck,0);
            if (*NextAck!=0) continue; // the failed availability assertion is already decisive; avoid a full-queue recovery timeout
            Rig.Client.WithOfflineQueue(TEXT("")); Rig.Client.WithOfflineQueue(TEXT("cleanup"),Policy);
            if (!TestTrue(TEXT("normal reopen recovery drains"),Rig.Drain())) return false;
            const auto Recovered=MakeShared<int32,ESPMode::ThreadSafe>(0);
            Rig.Client.Put<void>(TEXT("/recovered")).Body(TEXT("safe")).QueueWhenOffline().OnQueued([Recovered](const FString&){++*Recovered;}).Send();
            if (!TestTrue(TEXT("reopen removes pending and allows admission"),PumpUntil([Recovered]{return *Recovered==1;}))) return false;
        }
        else TestEqual(TEXT("known-clean cleanup leaves admission usable"),*NextAck,1);
    }
    return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineCommittedOuterTest,"VaRest.Http.Offline.CommittedOuter",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineCommittedOuterTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing; using namespace VaRest::Http::OfflineTestPrivate;
    for (bool bOuter : {true,false})
    {
        FScopedPersistenceTest Disk; FTestRig Rig; FCleanup Cleanup(Rig); FScopedPersistenceHooks Hooks(Disk.Root);FScopedTraceRecorder Trace;
        Configure(Rig,Disk);if (!TestTrue(TEXT("loaded"),Disk.Drain())) return false;
        const auto Barrier=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();Hooks.Hooks->BeforeRename=Barrier;
        ON_SCOPE_EXIT {Barrier->Released=true;Rig.Drain();};
        const auto Outer=MakeShared<FAnswer,ESPMode::ThreadSafe>();if (bOuter) Rig.Client.Use(Outer,EVaRestStageSlot::Outer);
        const auto Outcome=MakeOutcome<void>();const auto Ack=MakeShared<int32,ESPMode::ThreadSafe>(0);
        const auto H=Observe(Rig.Client.Put<void>(TEXT("/committed-outer")).Body(TEXT("safe")).QueueWhenOffline(),Outcome).OnQueued([Ack](const FString&){++*Ack;}).Send();
        if (!TestTrue(TEXT("postcutoff rename held"),PumpUntil([Barrier]{return Barrier->Entered.load();}))) return false;
        TestFalse(TEXT("committing handle inactive"),H.IsActive());
        if (bOuter) {Outer->Answer();if (!TestTrue(TEXT("outer completes before rename"),Rig.Wait(Outcome))) return false;}
        else H.Cancel(); // postcutoff cancellation must not suppress truthful normal acknowledgement
        const int32 Before=Trace.Recorder->For(H.GetId()).Num();
        Barrier->Released=true;if (!TestTrue(TEXT("committed handback drains"),Rig.Drain())) return false;
        TestEqual(TEXT("real committed item retained"),Rig.Client.GetQueuedCount(),1);
        TestEqual(TEXT("one selected original outcome"),Outcome->CompleteCalls.load(),bOuter?1:0);
        TestEqual(TEXT("only normal admission acknowledges"),*Ack,bOuter?0:1);
        const auto Events=Trace.Recorder->For(H.GetId());
        if (bOuter) TestEqual(TEXT("no request metadata after sealed Outer completion"),Events.Num(),Before);
        int32 Notes=0,Completed=0;bool bSeenCompleted=false;
        for (const auto& Event:Events)
        {
            if (Event.Kind==EVaRestTraceKind::Completed)
            {++Completed;bSeenCompleted=true;if (!bOuter) TestTrue(TEXT("normal completed queued outcome"),Event.Payload.Get<FVaRestTraceCompleted>().Outcome==EVaRestTraceOutcome::Queued);}
            if (Event.Kind==EVaRestTraceKind::Note && Event.Payload.Get<FVaRestTraceNote>().Kind==EVaRestTraceNoteKind::Queued)
            {++Notes;TestFalse(TEXT("Queued precedes Completed"),bSeenCompleted);}
        }
        TestEqual(TEXT("one terminal metadata"),Completed,1);TestEqual(TEXT("Queued note only for open logical request"),Notes,bOuter?0:1);
        const auto Loaded=MakeShared<FQueueLoad,ESPMode::ThreadSafe>();
        FQueueStore::Load(FClientAccess::State(Rig.Client)->Queue->GetDirectory(),TEXT("https://api.test"),{},[Loaded](FQueueLoad&& R){*Loaded=MoveTemp(R);});
        TestTrue(TEXT("recovery drains"),Disk.Drain());TestTrue(TEXT("committed data recovers"),Loaded->bOk && Loaded->Items.Num()==1);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineCleanupStopTest,"VaRest.Http.Offline.CleanupStop",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineCleanupStopTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;using namespace VaRest::Http::Testing;using namespace VaRest::Http::OfflineTestPrivate;
    for (bool bAbortAdmitted : {false,true})
    {
        FScopedPersistenceTest Disk;
        {
            FScopedPersistenceRuntime Runtime;FTestRig Rig;FCleanup Cleanup(Rig);FScopedPersistenceHooks Hooks(Disk.Root);
            Configure(Rig,Disk);if (!TestTrue(TEXT("isolated load drains"),Disk.Drain())) return false;
            const auto Queue=FClientAccess::State(Rig.Client)->Queue;
            const auto Prepared=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();Hooks.Hooks->AfterPrepare=Prepared;
            const auto Block=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
            ON_SCOPE_EXIT {Prepared->Released=true;Block->Released=true;Rig.Drain();};
            const auto Outcome=MakeOutcome<void>();const auto Ack=MakeShared<int32,ESPMode::ThreadSafe>(0);
            const auto H=Observe(Rig.Client.Put<void>(TEXT("/stop-cleanup")).Body(TEXT("safe")).QueueWhenOffline(),Outcome).OnQueued([Ack](const FString&){++*Ack;}).Send();
            if (!TestTrue(TEXT("prepared before isolated stop"),PumpUntil([Prepared]{return Prepared->Entered.load();}))) return false;
            H.Cancel();if (!TestTrue(TEXT("logical cancel resolved independently"),Rig.Wait(Outcome))) return false;
            if (bAbortAdmitted)
            {
                TestTrue(TEXT("owned pipe blocker admitted"),LaunchPersistence([Block]{WaitPersistenceBarrier(Block);FIoResult R;R.bOk=true;return R;},[](FIoResult&&){}));
                const uint64 Before=PersistenceCounters().Launched.load();Prepared->Released=true;
                if (!TestTrue(TEXT("abort admitted behind real held pipe"),PumpUntil([Block,Before]{return Block->Entered.load() && PersistenceCounters().Launched.load()>Before;}))) return false;
                TestEqual(TEXT("cleanup reservation retained until handback"),Queue->Count(),1);
                TestEqual(TEXT("cleanup admission ticket retained"),Queue->NumAdmissionsForTest(),1);
            }
            Queue->StopForShutdown();Prepared->Released=true;Block->Released=true;Runtime.Stop();
            TestEqual(TEXT("Stop resolves physical reservation"),Queue->Count(),0);
            TestEqual(TEXT("Stop resolves admission ticket"),Queue->NumAdmissionsForTest(),0);
            TestTrue(TEXT("isolated executor fully drained"),PersistenceIdle());
            TestEqual(TEXT("all worker results consumed or discarded"),PersistenceCounters().Delivered.load(),PersistenceCounters().Launched.load());
            TestEqual(TEXT("no late acknowledgement during Stop"),*Ack,0);TestEqual(TEXT("one prompt canceled result"),Outcome->CompleteCalls.load(),1);
            const auto Abort=MakeShared<TOptional<bool>,ESPMode::ThreadSafe>();
            FQueueStore::Abort(Queue->GetDirectory(),99,TEXT("stopped"),[Abort](bool bOk){*Abort=bOk;});
            TestTrue(TEXT("refused cleanup returns explicit failure"),Abort->IsSet() && !Abort->GetValue());
        }
    }
    return true;
}


namespace VaRest::Http::Testing::PersistenceRegression
{
    class FCutoffCapture final : public IVaRestStage
    {
    public:
        FName GetStageName() const override {return TEXT("Regression.TerminalWitness");}
        void Process(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done) override
        {bEntered=true;Seen=Exchange;Next.Proceed(Exchange,MoveTemp(Done));}
        bool IsDone() const {const auto Pinned=Seen.Pin();return bEntered && (!Pinned.IsValid() || Pinned->IsDone());}
        bool bEntered=false;
        TWeakPtr<FVaRestExchange,ESPMode::ThreadSafe> Seen;
    };

    enum class ECutoffAction { Cancel, Deadline, WorkerCancel };
    bool Cutoff(FAutomationTestBase& Test,ECutoffAction Action)
    {
        for (bool bCommitted : {false,true})
        {
            FScopedPersistenceTest Disk; FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FCutoffCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer); FScopedPersistenceHooks Hooks(Disk.Root);
            auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
            if (bCommitted) Hooks.Hooks->BeforeRename=Gate; else Hooks.Hooks->AfterPrepare=Gate;
            ON_SCOPE_EXIT {Gate->Released=true; Rig.Client.CancelAll(); Rig.Drain();};
            Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),Disk.Root).WithOfflineQueue(TEXT("cutoff"));
            Rig.Connectivity->ReportOffline();
            const auto Probe=MakeShared<FOutcomeProbe,ESPMode::ThreadSafe>(); Rig.Client.Use(Probe,EVaRestStageSlot::Outer);
            if (!Test.TestTrue(TEXT("isolated queue loaded"),Rig.Drain())) return false;
            const int32 BaselineTimers=Rig.Clock->NumPendingTimers();
            const auto Reply=MakeOutcome<void>(); const auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
            auto Request=Rig.Client.Put<void>(TEXT("/save")).Body(TEXT("payload")).QueueWhenOffline();
            if (Action==ECutoffAction::Deadline) Request=MoveTemp(Request).Retry(FVaRestRetryPolicy::Disabled().WithDeadline(1));
            const auto Handle=Observe(MoveTemp(Request),Reply).OnQueued([Queued](const FString&){++*Queued;}).Send();
            if (!Test.TestTrue(TEXT("exact cutoff worker hold"),PumpUntil([Gate]{return Gate->Entered.load();}))) return false;
            Test.TestFalse(TEXT("truthful completion awaits rename"),Capture->IsDone());
            Test.TestEqual(TEXT("active until shared commit selection"),Handle.IsActive(),!bCommitted);
            Test.TestEqual(TEXT("no acknowledgement before rename"),*Queued,0);
            if (Action==ECutoffAction::Deadline) Rig.Clock->Advance(2);
            else if (Action==ECutoffAction::WorkerCancel)
            {
                const auto Returned=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
                auto Worker=UE::Tasks::Launch(TEXT("VaRest.Task9Cancel"),[Handle,Returned]{Handle.Cancel(); Returned->store(true);});
                if (!Test.TestTrue(TEXT("worker Cancel returned before release"),PumpUntil([Returned]{return Returned->load();}))) {Gate->Released=true;Worker.Wait();return false;}
                Worker.Wait();
            }
            else Handle.Cancel();
            if (!bCommitted)
            {
                if (!Test.TestTrue(TEXT("pregate public completion before release"),Rig.Wait(Reply))) return false;
                Test.TestEqual(TEXT("pregate terminal kind"),Reply->Error.Kind,Action==ECutoffAction::Deadline?EVaRestErrorKind::Timeout:EVaRestErrorKind::Canceled);
                Test.TestEqual(TEXT("pregate chain outcome before worker release"),Probe->Total(Handle.GetId()),1);
                if (Action!=ECutoffAction::Deadline)
                    Test.TestEqual(TEXT("prompt canceled CHAIN before release"),Probe->Count(Handle.GetId(),EVaRestTransportFailure::Canceled),1);
            }
            Gate->Released=true;
            if (!Test.TestTrue(TEXT("cutoff drains"),Rig.Drain())) return false;
            Test.TestFalse(TEXT("barrier never timed out"),Gate->TimedOut.load());
            Test.TestTrue(TEXT("actual outcome terminal"),Capture->IsDone());
            Test.TestEqual(TEXT("one outer outcome"),Probe->Total(Handle.GetId()),1);
            Test.TestEqual(TEXT("one truthful queued completion"),*Queued,bCommitted?1:0);
            Test.TestEqual(TEXT("ordinary callback only before cutoff"),Reply->CompleteCalls.load(),bCommitted?0:1);
            Test.TestEqual(TEXT("no physical wire"),Rig.Fake->NumCalls(),0);
            const auto State=FClientAccess::State(Rig.Client);
            Test.TestEqual(TEXT("exchange accounting settled"),State->NumInFlight(),0);
            Test.TestEqual(TEXT("admission tickets released"),State->Queue->NumAdmissionsForTest(),0);
            Test.TestEqual(TEXT("deadline disarmed"),Rig.Clock->NumPendingTimers(),BaselineTimers);
            auto Loaded=MakeShared<FQueueLoad,ESPMode::ThreadSafe>();
            FQueueStore::Load(QueueDirectory(Disk.Root,TEXT("cutoff")),TEXT("https://api.test"),{},[Loaded](FQueueLoad&& R){*Loaded=MoveTemp(R);});
            if (!Test.TestTrue(TEXT("actual recovery drained"),Disk.Drain())) return false;
            Test.TestTrue(TEXT("recovery successful"),Loaded->bOk);
            Test.TestEqual(TEXT("only committed record recoverable"),Loaded->Items.Num(),bCommitted?1:0);
        }
        return true;
    }
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineCancelAroundCommitTest,"VaRest.Http.Offline.CancelAroundCommit",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineCancelAroundCommitTest::RunTest(const FString& Parameters)
{ return VaRest::Http::Testing::PersistenceRegression::Cutoff(*this,VaRest::Http::Testing::PersistenceRegression::ECutoffAction::Cancel); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineDeadlineAroundCommitTest,"VaRest.Http.Offline.DeadlineAroundCommit",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineDeadlineAroundCommitTest::RunTest(const FString& Parameters)
{ return VaRest::Http::Testing::PersistenceRegression::Cutoff(*this,VaRest::Http::Testing::PersistenceRegression::ECutoffAction::Deadline); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineAnyThreadCancelAroundCommitTest,"VaRest.Http.Offline.AnyThreadCancelAroundCommit",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineAnyThreadCancelAroundCommitTest::RunTest(const FString& Parameters)
{ return VaRest::Http::Testing::PersistenceRegression::Cutoff(*this,VaRest::Http::Testing::PersistenceRegression::ECutoffAction::WorkerCancel); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineInterleavingsTest,"VaRest.Http.Offline.Interleavings",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineInterleavingsTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing;
    enum class EHold { Read, Prepare, Commit, Retire };
    enum class EAction { Cancel, Deadline, Clear, Session, Edges, Drop, DropClient, Shutdown };
    struct FRow { const TCHAR* Name; EHold Hold; EAction Action; EVaRestErrorKind Error; };
    const FRow Rows[]={
        {TEXT("cache cancel"),EHold::Read,EAction::Cancel,EVaRestErrorKind::Canceled},
        {TEXT("cache deadline"),EHold::Read,EAction::Deadline,EVaRestErrorKind::Timeout},
        {TEXT("cache clear"),EHold::Read,EAction::Clear,EVaRestErrorKind::None},
        {TEXT("cache account"),EHold::Read,EAction::Session,EVaRestErrorKind::None},
        {TEXT("prepare cancel"),EHold::Prepare,EAction::Cancel,EVaRestErrorKind::Canceled},
        {TEXT("prepare deadline"),EHold::Prepare,EAction::Deadline,EVaRestErrorKind::Timeout},
        {TEXT("prepare account"),EHold::Prepare,EAction::Session,EVaRestErrorKind::Auth},
        {TEXT("commit cancel"),EHold::Commit,EAction::Cancel,EVaRestErrorKind::None},
        {TEXT("retire online edges"),EHold::Retire,EAction::Edges,EVaRestErrorKind::None},
        {TEXT("retire dropped handle"),EHold::Retire,EAction::Drop,EVaRestErrorKind::None},
        {TEXT("retire released public client"),EHold::Retire,EAction::DropClient,EVaRestErrorKind::None},
        {TEXT("prepare shutdown"),EHold::Prepare,EAction::Shutdown,EVaRestErrorKind::None},
        {TEXT("commit shutdown"),EHold::Commit,EAction::Shutdown,EVaRestErrorKind::None}};
    for (const FRow& Row : Rows)
    {
        AddInfo(Row.Name);
        FScopedPersistenceTest Disk; auto Runtime=MakeUnique<FScopedPersistenceRuntime>(); FScopedCacheControls Controls;
        FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FCutoffCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer); FScopedPersistenceHooks Hooks(Disk.Root);
        const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
        ON_SCOPE_EXIT {Gate->Released=true; Rig.Client.CancelAll(); Rig.Drain();};
        auto Policy=FVaRestCachePolicy::MemoryAndDisk();Policy.MemoryBytes=0;Policy.DiskBytes=1024*1024;Policy.DefaultTtlSeconds=600;
        Rig.Client.WithBearer(TEXT("A-token")).WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),Disk.Root);
        auto State=FClientAccess::State(Rig.Client);
        const TWeakPtr<FClientState,ESPMode::ThreadSafe> WeakState=State;
        if (Row.Hold==EHold::Read) Rig.Client.WithCache(Policy);
        else {Rig.Client.WithOfflineQueue(TEXT("matrix"));Rig.Connectivity->ReportOffline();}
        if (!TestTrue(TEXT("row setup drained"),Rig.Drain())) return false;
        const auto Probe=MakeShared<FOutcomeProbe,ESPMode::ThreadSafe>(); Rig.Client.Use(Probe,EVaRestStageSlot::Outer);
        if (Row.Hold==EHold::Retire)
        {
            const auto Ack=MakeShared<int32,ESPMode::ThreadSafe>(0),Events=MakeShared<int32,ESPMode::ThreadSafe>(0);
            TUniquePtr<FTestRig> Survivor;
            const auto SurvivorEvents=MakeShared<TArray<FString>,ESPMode::ThreadSafe>();
            if (Row.Action==EAction::DropClient)
            {
                Survivor=MakeUnique<FTestRig>();
                Survivor->Client.WithBearer(TEXT("A-token")).WithConnectivity(Rig.Connectivity).WithClock(Rig.Clock)
                    .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),Disk.Root).WithOfflineQueue(TEXT("matrix"));
                Survivor->Client.OnQueuedResult().AddLambda([SurvivorEvents](const FVaRestQueuedResult& Result){SurvivorEvents->Add(Result.QueueId);});
                Survivor->Fake->Enqueue(FVaRestFakeResponse::Text(204,TEXT("")));
            }
            ON_SCOPE_EXIT {if (Survivor) {Gate->Released=true;Survivor->Client.CancelAll();Survivor->Drain();}};
            const auto Queue=State->Queue;
            const auto Original=MakeOutcome<void>();
            auto OriginalHandle=Observe(Rig.Client.Put<void>(TEXT("/one")).Body(TEXT("one")).NoAuth().QueueWhenOffline(),Original).OnQueued([Ack](const FString&){++*Ack;}).Send();
            Rig.Client.Put<void>(TEXT("/two")).Body(TEXT("two")).NoAuth().QueueWhenOffline().OnQueued([Ack](const FString&){++*Ack;}).Send();
            if (!TestTrue(TEXT("two records durably acknowledged"),PumpUntil([Ack]{return *Ack==2;})) || !TestTrue(TEXT("admission drained"),Rig.Drain())) return false;
            Rig.Client.OnQueuedResult().AddLambda([Events](const FVaRestQueuedResult&){++*Events;});
            Rig.Fake->bCompleteOnWorker=false;Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
            Rig.Fake->Enqueue(FVaRestFakeResponse::Text(204,TEXT("")));Rig.Fake->Enqueue(FVaRestFakeResponse::Text(204,TEXT("")));
            Rig.Connectivity->ReportOnline();
            if (!TestTrue(TEXT("first replay physical"),PumpUntil([&Rig]{return Rig.Fake->NumCalls()==1;}))) return false;
            PumpPersistence();TestEqual(TEXT("replay guard handed back"),PersistenceCounters().Launched.load(),PersistenceCounters().HandedBack.load());
            if (!TestTrue(TEXT("serial hold accepted"),LaunchPersistence([Gate]{WaitPersistenceBarrier(Gate);return FIoResult();},[](FIoResult&&){}))) return false;
            if (!TestTrue(TEXT("serial hold entered before retirement"),PumpUntil([Gate]{return Gate->Entered.load();}))) return false;
            const uint64 BeforeRetire=PersistenceCounters().Launched.load();
            Rig.Fake->Complete(0,FVaRestFakeResponse::Text(204,TEXT("")));
            if (!TestTrue(TEXT("actual retire admitted behind serial barrier"),PumpUntil([State,BeforeRetire]{return State->NumInFlight()==0 && PersistenceCounters().Launched.load()==BeforeRetire+1;}))) return false;
            TestEqual(TEXT("retirement not acknowledged early"),*Events,0); TestEqual(TEXT("head retained until retirement"),Rig.Client.GetQueuedCount(),2);
            if (Row.Action==EAction::Drop) OriginalHandle=FVaRestRequestHandle();
            else if (Row.Action==EAction::DropClient) {Rig.Client=FVaRestClient();State.Reset();}
            else {Rig.Connectivity->ReportOffline();Rig.Connectivity->ReportOnline();Rig.Connectivity->ReportOffline();Rig.Connectivity->ReportOnline();}
            Queue->ScheduleReplay();Queue->ScheduleReplay();PumpPersistence();PumpPersistence();
            TestEqual(TEXT("active runner blocks a second guard before retirement"),PersistenceCounters().Launched.load(),BeforeRetire+1);
            TestEqual(TEXT("next head has no premature physical wire"),Rig.Fake->NumCalls(),1);
            Gate->Released=true;
            if (!TestTrue(TEXT("all replay retirement settled"),Rig.Drain())) return false;
            TestEqual(TEXT("next head starts once"),Rig.Fake->NumCalls()+(Survivor?Survivor->Fake->NumCalls():0),2);
            TestEqual(TEXT("original subscriber only while registered"),*Events,Survivor?0:2);
            if (Survivor)
            {
                TestFalse(TEXT("departed state released after owned retirement"),WeakState.IsValid());
                TestEqual(TEXT("departed owner has only its original wire"),Rig.Fake->NumCalls(),1);
                TestEqual(TEXT("surviving owner starts next head once"),Survivor->Fake->NumCalls(),1);
                TestEqual(TEXT("surviving subscriber sees both heads"),SurvivorEvents->Num(),2);
                if (SurvivorEvents->Num()==2) TestTrue(TEXT("each durable id delivered once"),(*SurvivorEvents)[0]!=(*SurvivorEvents)[1]);
            }
            TestEqual(TEXT("no original typed callback"),Original->CompleteCalls.load(),0);TestEqual(TEXT("queue empty"),Queue->Count(),0);
        }
        else
        {
            TUniquePtr<FScopedCacheStoreHooks> ReadHooks;
            if (Row.Hold==EHold::Read)
            {
                Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("old-cache")));
                auto Seed=MakeOutcome<FString>();Observe(Rig.Client.Get<FString>(TEXT("/entry")),Seed).Send();
                if (!TestTrue(TEXT("seed cached bytes"),Rig.Wait(Seed)) || !TestTrue(TEXT("seed disk drained"),Rig.Drain())) return false;
                FString Origin;CanonicalOrigin(State->GetConfig()->BaseUrl,Origin);
                ReadHooks=MakeUnique<FScopedCacheStoreHooks>(FPaths::Combine(State->GetConfig()->CacheRoot,HashFields(TEXT("VaRest/cache-origin/v1"),TArray<FString>{Origin})));
                ReadHooks->Hooks->AfterRead=Gate; // immutable old bytes captured before ClearCache
            }
            else if (Row.Hold==EHold::Prepare) Hooks.Hooks->AfterPrepare=Gate;
            else Hooks.Hooks->BeforeRename=Gate;
            const auto Reply=MakeOutcome<FString>();const auto Ack=MakeShared<int32,ESPMode::ThreadSafe>(0);
            auto Request=Row.Hold==EHold::Read ? Rig.Client.Get<FString>(TEXT("/entry")) : Rig.Client.Put<FString>(TEXT("/operation")).Body(TEXT("safe")).QueueWhenOffline();
            if (Row.Action==EAction::Deadline) Request=MoveTemp(Request).Retry(FVaRestRetryPolicy::Disabled().WithDeadline(1));
            const auto Handle=Observe(MoveTemp(Request),Reply).OnQueued([Ack](const FString&){++*Ack;}).Send();
            if (!TestTrue(TEXT("row worker reached exact hold"),PumpUntil([Gate]{return Gate->Entered.load();}))) return false;
            TestEqual(TEXT("no early queued event"),*Ack,0);
            if (Row.Action==EAction::Cancel) Handle.Cancel();
            if (Row.Action==EAction::Deadline) Rig.Clock->Advance(2);
            if (Row.Action==EAction::Clear) Rig.Client.ClearCache();
            if (Row.Action==EAction::Session) Rig.Client.SetBearerToken(TEXT("B-token"));
            if (Row.Action==EAction::Cancel && Row.Hold!=EHold::Commit)
            {
                if (!TestTrue(TEXT("public canceled before worker release"),Rig.Wait(Reply))) return false;
                TestEqual(TEXT("one canceled CHAIN before worker release"),Probe->Count(Handle.GetId(),EVaRestTransportFailure::Canceled),1);
            }
            if (Row.Action==EAction::Deadline && !TestTrue(TEXT("deadline completed before worker release"),Rig.Wait(Reply))) return false;
            if (Row.Action==EAction::Shutdown)
            {
                State->CancelAll(NAME_None,EVaRestCancelReason::Shutdown);State->Queue->StopForShutdown();Gate->Released=true;Runtime->Stop();
                TestEqual(TEXT("shutdown internal accounting settled"),State->NumInFlight(),0);TestTrue(TEXT("shutdown handle terminal"),Capture->IsDone());
                TestEqual(TEXT("shutdown silent public outcome"),Reply->CompleteCalls.load(),0);TestEqual(TEXT("shutdown silent queued event"),*Ack,0);
                TestEqual(TEXT("no shutdown global wrapper"),State->PendingApiCompletionWrappers,0);
                Runtime.Reset(); // restore normal executor only after all owned-stop assertions
            }
            else
            {
                if (Row.Hold==EHold::Read && (Row.Action==EAction::Clear || Row.Action==EAction::Session)) Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("new-wire")));
                Gate->Released=true;if (!TestTrue(TEXT("row drained"),Rig.Drain())) return false;
                TestEqual(TEXT("one final outer outcome"),Probe->Total(Handle.GetId()),1);
                if (Row.Hold==EHold::Commit) {TestEqual(TEXT("commit acknowledged once"),*Ack,1);TestEqual(TEXT("no ordinary queued result"),Reply->CompleteCalls.load(),0);}
                else
                {
                    TestEqual(TEXT("ordinary completion once"),Reply->CompleteCalls.load(),1);TestEqual(TEXT("expected terminal kind"),Reply->Error.Kind,Row.Error);
                    if (Row.Error==EVaRestErrorKind::None)
                    {
                        if (!TestTrue(TEXT("wire body available"),Reply->Value.IsSet())) return false;
                        TestEqual(TEXT("new identity/generation bytes"),Reply->Value.GetValue(),FString(TEXT("new-wire")));TestFalse(TEXT("old cache never reused"),Reply->Response.bFromCache);
                    }
                }
            }
            TestEqual(TEXT("no retained exchange"),State->NumInFlight(),0);
            if (Row.Hold==EHold::Read)
            {
                const bool bNew=Row.Action==EAction::Clear || Row.Action==EAction::Session;
                TestEqual(TEXT("wire count relative to seed"),Rig.Fake->NumCalls(),bNew?2:1);
                ReadHooks.Reset();
                if (bNew)
                {
                    // The held request retains its old snapshot: its later wire result may not populate
                    // the replacement generation/account. Only a new resolved operation can do that.
                    const auto Files=MakeShared<int32,ESPMode::ThreadSafe>(0);
                    FString Origin;CanonicalOrigin(State->GetConfig()->BaseUrl,Origin);
                    const FString Directory=FPaths::Combine(State->GetConfig()->CacheRoot,HashFields(TEXT("VaRest/cache-origin/v1"),TArray<FString>{Origin}));
                    LaunchPersistence([Directory]{return ListFiles(Directory);},[Files](FIoResult&& R){for (const FString& Name:R.Names) if (Name.EndsWith(TEXT(".json"))) ++*Files;});
                    if (!TestTrue(TEXT("old snapshot file inspection"),Disk.Drain())) return false;
                    TestEqual(TEXT("clear removes old generation; account replacement preserves only old account record"),*Files,Row.Action==EAction::Clear?0:1);
                    Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("new-wire")));
                    auto Fresh=MakeOutcome<FString>();Observe(Rig.Client.Get<FString>(TEXT("/entry")),Fresh).Send();
                    if (!TestTrue(TEXT("new resolved operation fills current generation"),Rig.Wait(Fresh)) || !TestTrue(TEXT("current generation persisted"),Rig.Drain())) return false;
                    TestFalse(TEXT("new identity had no stale record"),Fresh->Response.bFromCache);
                }
                auto Again=MakeOutcome<FString>();Observe(Rig.Client.Get<FString>(TEXT("/entry")),Again).Send();
                if (!TestTrue(TEXT("cache file still readable"),Rig.Wait(Again)) || !TestTrue(TEXT("file control drained"),Rig.Drain())) return false;
                TestTrue(TEXT("correct retained generation is cached"),Again->Response.bFromCache);
                if (!TestTrue(TEXT("retained value present"),Again->Value.IsSet())) return false;
                TestEqual(TEXT("retained file bytes"),Again->Value.GetValue(),FString(bNew?TEXT("new-wire"):TEXT("old-cache")));
            }
            else
            {
                TestEqual(TEXT("offline no transport"),Rig.Fake->NumCalls(),0);
                auto Loaded=MakeShared<FQueueLoad,ESPMode::ThreadSafe>();
                FQueueStore::Load(QueueDirectory(Disk.Root,TEXT("matrix")),TEXT("https://api.test"),{},[Loaded](FQueueLoad&& R){*Loaded=MoveTemp(R);});
                if (!TestTrue(TEXT("restart cleanup and recovery"),Disk.Drain())) return false;
                TestTrue(TEXT("actual files recoverable"),Loaded->bOk);TestEqual(TEXT("only successful rename survives"),Loaded->Items.Num(),Row.Hold==EHold::Commit?1:0);
            }
        }
        TestFalse(TEXT("exact barrier never timed out"),Gate->TimedOut.load());
        if (State) TestEqual(TEXT("all request state released"),State->NumInFlight(),0);
        TestEqual(TEXT("no game-thread filesystem operations"),PersistenceCounters().OperationsOnGameThread.load(),uint64(0));
    }
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflineScopeQueuePriorityTest,"VaRest.Http.Offline.ScopeQueuePriority",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflineScopeQueuePriorityTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;using namespace VaRest::Http::Testing;using namespace VaRest::Http::OfflineTestPrivate;
    FScopedPersistenceTest Disk;FTestRig A,B;FCleanup CleanA(A),CleanB(B);
    const auto Shared=A.Connectivity;
    A.Client.WithHealthUrl(TEXT("https://api.test/health"));
    B.Client.WithConnectivity(Shared).WithClock(A.Clock).WithStorageDirectories(Disk.Root,Disk.Root).WithOfflineQueue(TEXT("priority"));
    Shared->ReportOffline();
    const auto Ack=MakeShared<int32,ESPMode::ThreadSafe>(0);
    B.Client.Put<void>(TEXT("/queued")).Body(TEXT("safe")).QueueWhenOffline().OnQueued([Ack](const FString&){++*Ack;}).Send();
    if (!TestTrue(TEXT("durable queue seed"),PumpUntil([Ack]{return *Ack==1;})) || !TestTrue(TEXT("seed drain"),B.Drain())) return false;
    B.Client.WithOfflineQueue(TEXT(""));B.Client.WithOfflineQueue(TEXT("priority"));
    if (!TestTrue(TEXT("reopened queue load"),B.Drain()) || !TestEqual(TEXT("loaded durable head"),B.Client.GetQueuedCount(),1)) return false;
    const auto Results=MakeShared<int32,ESPMode::ThreadSafe>(0);B.Client.OnQueuedResult().AddLambda([Results](const FVaRestQueuedResult&){++*Results;});
    A.Fake->Enqueue(FVaRestFakeResponse::Manual());B.Fake->Enqueue(FVaRestFakeResponse::Manual());
    A.Clock->Advance(10);
    if (!TestTrue(TEXT("first physical probe starts"),PumpUntil([&A,&B]{return A.Fake->NumCalls()+B.Fake->NumCalls()>0;}))) return false;
    const bool bQueueFirst=TestEqual(TEXT("scope queue precedes older health owner"),B.Fake->NumCalls(),1);
    TestEqual(TEXT("health not dispatched while queue eligible"),A.Fake->NumCalls(),0);
    if (!bQueueFirst) return false;
    TestEqual(TEXT("actual queued application route"),B.Fake->GetCall(0).Request.Url,FString(TEXT("https://api.test/queued")));
    A.Clock->Advance(10);PumpPersistence();
    TestEqual(TEXT("held probe has no overlap"),A.Fake->NumCalls()+B.Fake->NumCalls(),1);
    B.Fake->Complete(0,FVaRestFakeResponse::Text(200,TEXT("replayed")));
    if (!TestTrue(TEXT("replay retired"),PumpUntil([Results]{return *Results==1;})) || !TestTrue(TEXT("terminal drain"),B.Drain())) return false;
    TestEqual(TEXT("queue empty"),B.Client.GetQueuedCount(),0);
    Shared->ReportOffline();A.Clock->Advance(10);
    if (!TestTrue(TEXT("health fallback starts"),PumpUntil([&A]{return A.Fake->NumCalls()==1;}))) return false;
    TestEqual(TEXT("health fallback route"),A.Fake->GetCall(0).Request.Url,FString(TEXT("https://api.test/health")));
    A.Fake->Complete(0,FVaRestFakeResponse::Text(200,TEXT("healthy")));
    return TestTrue(TEXT("scope drained after retirement and health"),A.Drain() && B.Drain());
}
#if PLATFORM_LINUX
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOfflinePrepareCleanupTest,"VaRest.Http.Offline.PrepareCleanup",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestOfflinePrepareCleanupTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;using namespace VaRest::Http::Testing;using namespace VaRest::Http::OfflineTestPrivate;
    for (int32 Mode=0;Mode<3;++Mode) // clean write failure, already-absent cleanup, actual denied unlink
    {
        AddInfo(FString::Printf(TEXT("prepare cleanup mode %d"),Mode));
        FScopedPersistenceTest Disk;FTestRig Rig;FCleanup Cleanup(Rig);FScopedPersistenceHooks Hooks(Disk.Root);
        FVaRestOfflinePolicy Policy;Policy.MaxQueued=1;Configure(Rig,Disk,TEXT("prepare-cleanup"),Policy);
        if (!TestTrue(TEXT("loaded"),Disk.Drain())) return false;
        const FString Directory=FClientAccess::State(Rig.Client)->Queue->GetDirectory();
        const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();Hooks.Hooks->BeforePrepareCleanup=Gate;Hooks.Hooks->bFailPrepare=true;
        ON_SCOPE_EXIT {FileTask([Directory]{chmod(TCHAR_TO_UTF8(*PersistencePathForIo(Directory)),0700);});Gate->Released=true;Rig.Client.CancelAll();TestTrue(TEXT("prepare cleanup unwind"),Rig.Drain());};
        const auto Ack=MakeShared<int32,ESPMode::ThreadSafe>(0);auto Failed=MakeOutcome<void>();
        Observe(Rig.Client.Put<void>(TEXT("/prepare")).Body(TEXT("safe")).QueueWhenOffline(),Failed).OnQueued([Ack](const FString&){++*Ack;}).Send();
        if (!TestTrue(TEXT("write completed and handle closed before cleanup"),PumpUntil([Gate]{return Gate->Entered.load();}))) return false;
        const auto Part=MakeShared<FString,ESPMode::ThreadSafe>();const auto Witness=MakeShared<bool,ESPMode::ThreadSafe>(false);
        if (!TestTrue(TEXT("physical witness task"),FileTask([Directory,Part,Witness,Mode]
        {
            const auto Files=ListFiles(Directory);if (!Files.bOk || Files.Names.Num()!=1 || !Files.Names[0].EndsWith(TEXT(".part"))) return;
            *Part=FPaths::Combine(Directory,Files.Names[0]);const auto Status=InspectPersistencePath(*Part);
            if (Status.Kind!=EPathKind::Regular || Status.Size==0) return;
            if (Mode==1) *Witness=DeleteFile(*Part).bOk && InspectPersistencePath(*Part).Kind==EPathKind::Missing;
            else if (Mode==2) *Witness=chmod(TCHAR_TO_UTF8(*PersistencePathForIo(Directory)),0500)==0 && !DeleteFile(*Part).bOk && InspectPersistencePath(*Part).Kind==EPathKind::Regular;
            else *Witness=true;
        })) || !TestTrue(TEXT("actual nonempty part / absent / denied unlink"),*Witness)) return false;
        const auto Flushed=MakeShared<TOptional<bool>,ESPMode::ThreadSafe>();TestTrue(TEXT("flush admitted"),Rig.Client.FlushQueue([Flushed](bool bOk){*Flushed=bOk;}));TestFalse(TEXT("flush noninline"),Flushed->IsSet());
        Gate->Released=true;if (!TestTrue(TEXT("failed prepare drained"),Rig.Drain())) return false;
        TestEqual(TEXT("failed prepare never acked"),*Ack,0);TestEqual(TEXT("one failed completion"),Failed->CompleteCalls.load(),1);
        if (!TestTrue(TEXT("flush result"),Flushed->IsSet())) return false;TestFalse(TEXT("write failure reaches flush"),Flushed->GetValue());
        FileTask([Directory,Part,Witness,Mode]
        {
            *Witness=InspectPersistencePath(*Part).Kind==(Mode==2?EPathKind::Regular:EPathKind::Missing);
            *Witness=chmod(TCHAR_TO_UTF8(*PersistencePathForIo(Directory)),0700)==0 && *Witness;
        });
        if (!TestTrue(TEXT("residual truth and permissions restored before next admission"),*Witness)) return false;
        Hooks.Hooks->bFailPrepare=false; // prior captured worker and callbacks are fully drained
        const uint64 Before=PersistenceCounters().Launched.load();const auto NextAck=MakeShared<int32,ESPMode::ThreadSafe>(0);auto Next=MakeOutcome<void>();
        Observe(Rig.Client.Put<void>(TEXT("/next")).Body(TEXT("safe")).QueueWhenOffline(),Next).OnQueued([NextAck](const FString&){++*NextAck;}).Send();
        if (!TestTrue(TEXT("next resolves after IO restored"),Rig.Drain())) return false;
        if (Mode==2)
        {
            TestEqual(TEXT("uncertain prepare cleanup refuses new IO"),PersistenceCounters().Launched.load(),Before);
            TestEqual(TEXT("uncertain prepare cleanup cannot acknowledge"),*NextAck,0);
            TestEqual(TEXT("storage unavailable"),Next->Error.Kind,EVaRestErrorKind::Config);
            if (*NextAck!=0) continue;
            Rig.Client.WithOfflineQueue(TEXT(""));Rig.Client.WithOfflineQueue(TEXT("prepare-cleanup"),Policy);
            if (!TestTrue(TEXT("fully released coordinator recovers"),Rig.Drain())) return false;
            FileTask([Part,Witness]{*Witness=InspectPersistencePath(*Part).Kind==EPathKind::Missing;});TestTrue(TEXT("Load removes orphan part"),*Witness);
            const auto Recovered=MakeShared<int32,ESPMode::ThreadSafe>(0);
            Rig.Client.Put<void>(TEXT("/recovered")).Body(TEXT("safe")).QueueWhenOffline().OnQueued([Recovered](const FString&){++*Recovered;}).Send();
            if (!TestTrue(TEXT("recovered admission"),PumpUntil([Recovered]{return *Recovered==1;}))) return false;
        }
        else TestEqual(TEXT("known clean failure permits admission"),*NextAck,1);
    }
    return true;
}
#endif

#endif
