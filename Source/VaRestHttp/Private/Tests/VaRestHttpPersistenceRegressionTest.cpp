// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS
#include "VaRestHttpTesting.h"
#include "VaRestClientState.h"
#include "VaRestExchangeState.h"
#include "VaRestPersistence.h"
#include "VaRestDiskCache.h"
#include "VaRestQueueStore.h"
#include "VaRestOfflineInternal.h"
#include "VaRestCachePolicy.h"
#include "VaRestIdentity.h"
#include "VaRestJson.h"
#include "VaRestAuth.h"
#include "HAL/PlatformFileManager.h"
#include "Tasks/Task.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"

namespace VaRest::Http::Testing::PersistenceRegression
{
    class FExchangeCapture final : public IVaRestStage
    {
    public:
        FName GetStageName() const override {return TEXT("Regression.TerminalWitness");}
        void Process(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done) override
        {bEntered=true;Seen=Exchange;Next.Proceed(Exchange,MoveTemp(Done));}
        bool IsDone() const {const auto Pinned=Seen.Pin();return bEntered && (!Pinned.IsValid() || Pinned->IsDone());}
        bool bEntered=false;
        TWeakPtr<FVaRestExchange,ESPMode::ThreadSafe> Seen;
    };

    FQueueItem SafeItem(uint64 Sequence,int32 BodyBytes=1024)
    {
        FQueueItem Item; Item.QueueId=FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);
        Item.Sequence=Sequence; Item.OriginalRun=QueueRunId(); Item.OriginalRequestId=Sequence;
        Item.CreatedUtc=FDateTime::UtcNow(); Item.Scope=HashFields(TEXT("regression"),TArray<FString>{TEXT("anonymous")});
        Item.Verb=TEXT("PUT"); Item.Url=TEXT("https://api.test/items"); Item.AuthMode=EVaRestAuthMode::None;
        TArray<uint8> Bytes; Bytes.Init(71,BodyBytes);
        Item.Body=FVaRestRequestBody::FromBytes(MoveTemp(Bytes),EVaRestBodyKind::Binary,TEXT("application/octet-stream"));
        Item.Headers.Set(TEXT("Content-Type"),Item.Body.ContentType);
        return Item;
    }
    FCacheEntry SafeEntry()
    {
        FCacheEntry Entry; Entry.Key=HashFields(TEXT("regression/cache"),TArray<FString>{TEXT("entry")});
        Entry.Body={71,72,73}; Entry.ContentType=TEXT("application/octet-stream");
        Entry.Headers.Set(TEXT("Cache-Control"),TEXT("max-age=600"));
        Entry.Metadata=ReadCacheMetadata(Entry.Headers,0,0,FDateTime::UtcNow());
        return Entry;
    }
    class FDelayedTimerClock final : public IVaRestClock
    {
    public:
        double Now() const override { return Seconds.load(); }
        FVaRestTimerHandle SetTimer(double Delay,TUniqueFunction<void()>&& Callback) override
        { return Timers.SetTimer(Delay,MoveTemp(Callback)); }
        void ClearTimer(FVaRestTimerHandle& Handle) override { Timers.ClearTimer(Handle); }
        void AdvanceNowOnly(double Delta) { Seconds.store(Seconds.load()+Delta); }
        int32 Pending() const { return Timers.NumPendingTimers(); }
    private:
        std::atomic<double> Seconds{0};
        FVaRestVirtualClock Timers;
    };
    class FEarlyOuter final : public IVaRestStage,public TSharedFromThis<FEarlyOuter,ESPMode::ThreadSafe>
    {
    public:
        FName GetStageName() const override { return TEXT("Regression.EarlyOuter"); }
        void Process(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done) override
        {
            Pending=MoveTemp(Done);
            Next.Proceed(Exchange,[Self=AsShared()](FVaRestAttemptOutcome&& Outcome)
            {
                ++Self->InnerCalls;
                if (Self->Pending) { auto Finish=MoveTemp(Self->Pending); Finish(MoveTemp(Outcome)); }
            });
        }
        void FinishNow()
        { check(Pending); auto Finish=MoveTemp(Pending); FVaRestAttemptOutcome Answer; Answer.Status=204; Finish(MoveTemp(Answer)); }
        int32 InnerCalls=0;
    private:
        FVaRestOutcomeCallback Pending;
    };
}
namespace VaRest::Http::Testing::PersistenceRegression
{
class FLogicalReplayFailure final : public IVaRestStage
{
public:
    FName GetStageName() const override { return TEXT("Regression.LogicalReplayFailure"); }
    void Process(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done) override
    {
        if (VaRest::Http::Internals(*Exchange).QueueReplayOf.QueueId.IsEmpty())
        { Next.Proceed(Exchange,MoveTemp(Done)); return; }
        ++Entered; Pending=MoveTemp(Done);
        if (!bHold) Fail();
    }
    void Fail()
    {
        check(Pending); auto Finish=MoveTemp(Pending);
        FVaRestAttemptOutcome Result; Result.Failure=EVaRestTransportFailure::Connection; Finish(MoveTemp(Result));
    }
    bool bHold=false;
    int32 Entered=0;
private:
    FVaRestOutcomeCallback Pending;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionCaseDistinctSecretsTest,"VaRest.Http.PersistenceRegression.CaseDistinctSecrets", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionCaseDistinctSecretsTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
Rig.Client.WithBaseUrl(TEXT("https://api.test"))
    .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
    .WithCache(FVaRestCachePolicy::MemoryAndDisk()).WithOfflineQueue(TEXT("secret-case"));
Rig.Client.SetSecretValue(TEXT("case-one"),TEXT("AbCdEfSecret"));
Rig.Client.SetSecretValue(TEXT("case-two"),TEXT("aBcDeFsECRET"));
if (!TestTrue(TEXT("load drained"),Rig.Drain())) return false;
for (const TCHAR* Secret : {TEXT("AbCdEfSecret"),TEXT("aBcDeFsECRET")})
{
    Rig.Connectivity->ReportOnline();
    Rig.Client.ClearCache(); if (!TestTrue(TEXT("clear drained"),Rig.Drain())) return false;
    Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,Secret));
    const auto Reply=MakeOutcome<FString>();
    Observe(Rig.Client.Get<FString>(TEXT("/secret-response")),Reply).Send();
    if (!TestTrue(TEXT("response completes"),Rig.Wait(Reply))) return false;
    TestEqual(TEXT("live response preserved"),Reply->SuccessCalls.load(),1);
    Rig.Connectivity->ReportOffline();
    const auto Rejected=MakeOutcome<void>();
    const auto QueueCalls=MakeShared<int32,ESPMode::ThreadSafe>(0);
    Observe(Rig.Client.Put<void>(TEXT("/items")).Body(Secret).QueueWhenOffline(),Rejected)
        .OnQueued([QueueCalls](const FString&){++*QueueCalls;}).Send();
    if (!TestTrue(TEXT("queue decision delivered"),PumpUntil([Rejected,QueueCalls]{return Rejected->IsDone() || *QueueCalls>0;}))) return false;
    TestEqual(TEXT("secret never admitted"),*QueueCalls,0);
    TestTrue(TEXT("persistence failure"),Rejected->Error.Kind==EVaRestErrorKind::Config);
}
if (!TestTrue(TEXT("writes drained"),Rig.Drain())) return false;
const auto Files=MakeShared<int32,ESPMode::ThreadSafe>(0);
LaunchPersistence([Root=Disk.Root]
{
    FIoResult Result; Result.bOk=true;
    FPlatformFileManager::Get().GetPlatformFile().IterateDirectoryRecursively(*Root,
        [&Result](const TCHAR* Path,bool bDirectory)
        { if (!bDirectory) Result.Names.Add(FString(Path)); return true; });
    return Result;
},[Files](FIoResult&& Result){ *Files=Result.Names.Num(); });
if (!TestTrue(TEXT("inspection drained"),Disk.Drain())) return false;
TestEqual(TEXT("neither case variant wrote even a temporary file"),*Files,0);
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionQueueCodecSplitTest,"VaRest.Http.PersistenceRegression.QueueCodecSplit", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionQueueCodecSplitTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing::PersistenceRegression;
auto Item=SafeItem(1,1); Item.Body.Bytes[0]=77;
Item.Tag=TEXT("quoted \\\"body\\\":\\\"TQ==\\\" text");
const auto Encoded=EncodeQueueRecord(Item);
auto Good=DecodeQueueRecord(Encoded,TEXT("https://api.test"),FVaRestOfflinePolicy());
if (!TestTrue(TEXT("quoted metadata cannot delimit body"),Good.HasValue())) return false;
TestTrue(TEXT("bytes survive split"),Good.GetValue().Body.Bytes==Item.Body.Bytes);
const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Encoded.GetData()),Encoded.Num());
const FString Text(Converted.Length(),Converted.Get());
for (const TCHAR* Replacement : {
    TEXT("\"body\":\"TQ==\",\"body\":\"\""),
    TEXT("\"bo\\u0064y\":\"TQ==\""),
    TEXT("\"body\":\"\\u0054Q==\""),
    TEXT("\"body\":\"TQ=A\""),
    TEXT("\"body\":\"TR==\""),
    TEXT("\"body\":\"TQ=\""),
    TEXT("\"body\":\"TQ==\",\"bo\\u0064y\":\"\"")})
{
    const FString Bad=Text.Replace(TEXT("\"body\":\"TQ==\""),Replacement,ESearchCase::CaseSensitive);
    if (!TestFalse(TEXT("fixture replacement happened"),Bad.Equals(Text,ESearchCase::CaseSensitive))) return false;
    const FTCHARToUTF8 Utf8(*Bad);
    TestTrue(TEXT("ambiguous or noncanonical body refused"),DecodeQueueRecord(
        MakeArrayView(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length()),TEXT("https://api.test"),FVaRestOfflinePolicy()).HasError());
}
for (int32 Size : {0,1,2,3,1024*1024,32*1024*1024})
{
    Testing::FScopedQueueCodecBuffers Probe;
    {
    auto Roundtrip=SafeItem(2,Size);
    Testing::FQueueCodecBufferOwner CallerBody(Testing::EQueueCodecAccount::Body,Roundtrip.Body.Bytes);
    const auto Bytes=EncodeQueueRecord(Roundtrip);
    if (!TestTrue(TEXT("valid record encoded"),!Bytes.IsEmpty())) return false;
    Testing::FQueueCodecBufferOwner CallerEncoded(Testing::EQueueCodecAccount::Encoded,Bytes);
    auto Decoded=DecodeQueueRecord(Bytes,TEXT("https://api.test"),FVaRestOfflinePolicy());
    if (!TestTrue(TEXT("canonical lengths decode"),Decoded.HasValue())) return false;
    Testing::FQueueCodecBufferOwner ReturnedBody(Testing::EQueueCodecAccount::Body,Decoded.GetValue().Body.Bytes);
    TestTrue(TEXT("canonical body exact"),Decoded.GetValue().Body.Bytes==Roundtrip.Body.Bytes);
    TestTrue(TEXT("actual encoded backing observed separately"),Probe.Buffers.Peak[0]>=int64(Bytes.GetAllocatedSize()));
    TestTrue(TEXT("caller and returned body backing observed separately"),Probe.Buffers.Peak[1]>=2LL*Size);
    TestTrue(TEXT("additional backing buffers within two MiB"),Probe.Buffers.Peak[2]>0 && Probe.Buffers.Peak[2]<=2LL*1024*1024);
    TestEqual(TEXT("all additional owner tokens released"),Probe.Buffers.Current[2],int64(0));
    }
    for (int32 Account=0;Account<3;++Account) TestEqual(TEXT("all real buffer owners released"),Probe.Buffers.Current[Account],int64(0));
}
// These malformed inputs must fail in the bounded scan before metadata allocation.
for (int32 Shape=0;Shape<3;++Shape)
{
    FString Extra;
    if (Shape==0) Extra=TEXT(",\"extra\":\"")+FString::ChrN(65536,TEXT('x'))+TEXT("\"");
    if (Shape==1) { Extra=TEXT(",\"extra\":[0"); for (int32 I=1;I<8192;++I) Extra+=TEXT(",0"); Extra+=TEXT("]"); }
    if (Shape==2) Extra=TEXT(",\"extra\":[[[[[[[[[0]]]]]]]]]");
    const FString Bad=Text.LeftChop(1)+Extra+TEXT("}"); const FTCHARToUTF8 Utf8(*Bad);
    Testing::FScopedQueueCodecBuffers Probe;
    TestTrue(TEXT("excess metadata units and depth refused before DOM"),DecodeQueueRecord(
        MakeArrayView(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length()),TEXT("https://api.test"),{}).HasError());
    TestEqual(TEXT("refused scan allocates no metadata backing"),Probe.Buffers.Peak[2],int64(0));
}
// Measured owners exclude the native JSON arena; its bounded static allowance is separate.

return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionRecoveryBoundsTest,"VaRest.Http.PersistenceRegression.RecoveryBounds", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionRecoveryBoundsTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
using namespace VaRest::Http::Testing::PersistenceRegression;
FScopedPersistenceTest Disk;
const FString Root=FPaths::Combine(Disk.Root,TEXT("records"));
const auto Written=MakeShared<int32,ESPMode::ThreadSafe>(0);
for (uint64 Sequence=1;Sequence<=4;++Sequence)
{
    const auto Item=SafeItem(Sequence);
    LaunchPersistence([Root,Item]{ return WriteNewFile(FPaths::Combine(Root,QueueFileName(Item.Sequence,Item.QueueId)),EncodeQueueRecord(Item)); },
        [Written](FIoResult&& Result){ if (Result.bOk) ++*Written; });
}
if (!TestTrue(TEXT("fixture writes drained"),Disk.Drain())) return false;
if (!TestEqual(TEXT("four valid acknowledged-format records"),*Written,4)) return false;
for (bool bCountLimit : {false,true})
{
    FVaRestOfflinePolicy Lower; Lower.MaxItemBytes=1024; Lower.MaxBodyBytes=bCountLimit ? 4096 : 2048; Lower.MaxQueued=bCountLimit ? 2 : 256;
    const auto Loaded=MakeShared<FQueueLoad,ESPMode::ThreadSafe>();
    FQueueStore::Load(Root,TEXT("https://api.test"),Lower,[Loaded](FQueueLoad&& Value){ *Loaded=MoveTemp(Value); });
    if (!TestTrue(TEXT("bounded recovery drained"),Disk.Drain())) return false;
    TestFalse(TEXT("incompatible policy refused"),Loaded->bOk);
    TestTrue(TEXT("policy reason distinguished"),Loaded->bPolicyExceeded);
    TestEqual(TEXT("no partial replay window"),Loaded->Items.Num(),0);
    TestTrue(TEXT("peak retained bodies bounded before append"),Loaded->PeakBodyBytes<=Lower.MaxBodyBytes);
    TestTrue(TEXT("metadata including candidate bounded"),Loaded->PeakMetadataBytes<=10*1024*1024+256*1024);
}
FVaRestOfflinePolicy Adequate; Adequate.MaxItemBytes=1024; Adequate.MaxBodyBytes=4096;
const auto Reopened=MakeShared<FQueueLoad,ESPMode::ThreadSafe>();
FQueueStore::Load(Root,TEXT("https://api.test"),Adequate,[Reopened](FQueueLoad&& Value){ *Reopened=MoveTemp(Value); });
if (!TestTrue(TEXT("adequate recovery drained"),Disk.Drain())) return false;
if (!TestTrue(TEXT("valid files were preserved"),Reopened->bOk)) return false;
if (!TestEqual(TEXT("all acknowledged operations survive"),Reopened->Items.Num(),4)) return false;
for (int32 Index=0;Index<4;++Index) TestEqual(TEXT("FIFO preserved"),Reopened->Items[Index].Sequence,uint64(Index+1));
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionRecoveryMetadataTest,"VaRest.Http.PersistenceRegression.RecoveryMetadata", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionRecoveryMetadataTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
using namespace VaRest::Http::Testing::PersistenceRegression;
FScopedPersistenceTest Disk;
const FString Root=QueueDirectory(Disk.Root,TEXT("metadata"));
const auto Written=MakeShared<int32,ESPMode::ThreadSafe>(0);
for (uint64 Sequence=1;Sequence<=300;++Sequence)
{
    auto Item=SafeItem(Sequence,0);
    for (int32 Header=0;Header<4;++Header)
        Item.Headers.Set(FString::Printf(TEXT("X-Safe-%d"),Header),FString::ChrN(8000,TEXT('z')));
    LaunchPersistence([Root,Item]{ return WriteNewFile(FPaths::Combine(Root,QueueFileName(Item.Sequence,Item.QueueId)),EncodeQueueRecord(Item)); },
        [Written](FIoResult&& Result){ if (Result.bOk) ++*Written; },256*1024);
}
if (!TestTrue(TEXT("metadata fixture drained"),Disk.Drain())) return false;
if (!TestEqual(TEXT("complete bounded fixtures"),*Written,300)) return false;
FVaRestOfflinePolicy Policy; Policy.MaxQueued=512; Policy.MaxItemBytes=1; Policy.MaxBodyBytes=1;
const auto Loaded=MakeShared<FQueueLoad,ESPMode::ThreadSafe>();
FQueueStore::Load(Root,TEXT("https://api.test"),Policy,[Loaded](FQueueLoad&& Value){ *Loaded=MoveTemp(Value); });
if (!TestTrue(TEXT("metadata recovery drained"),Disk.Drain())) return false;
TestFalse(TEXT("metadata excess refused"),Loaded->bOk); TestTrue(TEXT("capacity reason"),Loaded->bPolicyExceeded);
TestEqual(TEXT("failed prefix never exposed"),Loaded->Items.Num(),0);
TestEqual(TEXT("no body material"),Loaded->PeakBodyBytes,int64(0));
TestTrue(TEXT("bounded metadata peak"),Loaded->PeakMetadataBytes<=10*1024*1024+256*1024);
const auto Files=MakeShared<int32,ESPMode::ThreadSafe>(0);
LaunchPersistence([Root]{ return ListFiles(Root); },[Files](FIoResult&& Result){ *Files=Result.Names.Num(); });
if (!TestTrue(TEXT("preservation inspection"),Disk.Drain())) return false;
TestEqual(TEXT("all valid records preserved"),*Files,300);
FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer); Rig.Connectivity->ReportOffline();
Rig.Client.WithBaseUrl(TEXT("https://api.test")).WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),Disk.Root)
    .WithOfflineQueue(TEXT("metadata"),Policy);
if (!TestTrue(TEXT("coordinator load"),Rig.Drain())) return false;
auto Reply=MakeOutcome<void>(); Observe(Rig.Client.Put<void>(TEXT("/items")).QueueWhenOffline(),Reply).Send();
if (!TestTrue(TEXT("unavailable store refuses admission"),Rig.Wait(Reply))) return false;
TestTrue(TEXT("Config result"),Reply->Error.Kind==EVaRestErrorKind::Config);
Rig.Connectivity->ReportOnline();
if (!TestTrue(TEXT("no partial replay"),Rig.Drain())) return false;
TestEqual(TEXT("no wire for unavailable recovery"),Rig.Fake->NumCalls(),0);
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionMetadataReservationsTest,"VaRest.Http.PersistenceRegression.MetadataReservations", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionMetadataReservationsTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
Rig.Connectivity->ReportOffline();
Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),Disk.Root)
    .WithOfflineQueue(TEXT("metadata-reservations"));
if (!TestTrue(TEXT("empty load complete"),Rig.Drain())) return false;
const auto Queue=FClientAccess::State(Rig.Client)->Queue;
if (!TestTrue(TEXT("registered"),Queue.IsValid())) return false;
FString Failure;
int32 Accepted=0;
for (int32 Index=0;Index<40;++Index)
{
    if (!Queue->Reserve(0,256*1024,Failure)) break;
    ++Accepted;
}
TestEqual(TEXT("ten MiB reserved"),Accepted,40);
const bool bOverflow=Queue->Reserve(0,256*1024,Failure);
TestFalse(TEXT("metadata overflow refused"),bOverflow);
TestEqual(TEXT("fixed refusal"),Failure,FString(TEXT("QueueFull")));
if (bOverflow) Queue->ReleaseReservation(0,256*1024);
for (int32 Index=0;Index<Accepted;++Index) Queue->ReleaseReservation(0,256*1024);
const bool bAfterRelease=Queue->Reserve(0,256*1024,Failure);
TestTrue(TEXT("capacity released"),bAfterRelease);
if (bAfterRelease) Queue->ReleaseReservation(0,256*1024);
TestEqual(TEXT("no stranded reservation"),Queue->Count(),0);
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionMetadataRestartParityTest,"VaRest.Http.PersistenceRegression.MetadataRestartParity", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionMetadataRestartParityTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
const FString CacheRoot=FPaths::Combine(Disk.Root,TEXT("cache")),QueueRoot=FPaths::Combine(Disk.Root,TEXT("queue"));
TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe> Previous;
int32 Accepted=0; int64 AcknowledgedCharge=0;
{
    FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer); Rig.Connectivity->ReportOffline();
    Rig.Client.WithBaseUrl(TEXT("https://api.test")).WithStorageDirectories(CacheRoot,QueueRoot).WithOfflineQueue(TEXT("parity"));
    if (!TestTrue(TEXT("production load complete"),Rig.Drain())) return false;
    const auto Queue=FClientAccess::State(Rig.Client)->Queue; Previous=Queue;
    for (int32 ValueLength : {6000,64})
    {
        for (int32 Header=0;Header<5;++Header)
            Rig.Client.WithHeader(FString::Printf(TEXT("X-Pair-%d"),Header),FString::ChrN(ValueLength,TEXT('v')));
        bool bRefused=false;
        for (int32 Attempt=0;Attempt<257;++Attempt)
        {
            auto Reply=MakeOutcome<void>(); const auto Queued=MakeShared<bool,ESPMode::ThreadSafe>(false);
            Observe(Rig.Client.Put<void>(TEXT("/items")).NoAuth().QueueWhenOffline(),Reply)
                .OnQueued([Queued](const FString&){ *Queued=true; }).Send();
            if (!TestTrue(TEXT("admission settles"),PumpUntil([Queued,Reply]{return *Queued || Reply->CompleteCalls.load()!=0;}))) return false;
            if (!*Queued)
            {
                TestTrue(TEXT("finite metadata refusal"),Reply->Error.Kind==EVaRestErrorKind::Config);
                bRefused=true; break;
            }
            ++Accepted;
        }
        if (!TestTrue(TEXT("finite ceiling reached"),bRefused)) return false;
    }
    if (!TestTrue(TEXT("all acknowledged writes drained"),Rig.Drain())) return false;
    TestTrue(TEXT("metadata rather than count ceiling"),Accepted>40 && Accepted<256);
    AcknowledgedCharge=Queue->MetadataBytesForTest();
    TestTrue(TEXT("near ten MiB ceiling"),AcknowledgedCharge>10*1024*1024-16384);
    TestTrue(TEXT("within ceiling"),AcknowledgedCharge<=10*1024*1024);
    TestEqual(TEXT("every acknowledgement retained"),Rig.Client.GetQueuedCount(),Accepted);
}
if (!TestFalse(TEXT("full coordinator release"),Previous.IsValid())) return false;
FTestRig Restored; Restored.Connectivity->ReportOffline();
Restored.Client.WithBaseUrl(TEXT("https://api.test")).WithStorageDirectories(CacheRoot,QueueRoot).WithOfflineQueue(TEXT("parity"));
if (!TestTrue(TEXT("same-policy recovery"),Restored.Drain())) return false;
TestEqual(TEXT("all acknowledged records available"),Restored.Client.GetQueuedCount(),Accepted);
const auto Queue=FClientAccess::State(Restored.Client)->Queue;
if (!TestTrue(TEXT("recovered coordinator"),Queue.IsValid())) return false;
TestTrue(TEXT("recovery does not grow acknowledged charge"),Queue->MetadataBytesForTest()<=AcknowledgedCharge);
for (int32 Index=0;Index<Accepted;++Index) Restored.Fake->Enqueue(FVaRestFakeResponse::Text(204,TEXT("")));
Restored.Connectivity->ReportOnline();
if (!TestTrue(TEXT("recovered records retire"),PumpUntil([&Restored]{return Restored.Client.GetQueuedCount()==0;}))) return false;
if (!TestTrue(TEXT("retirement drained"),Restored.Drain())) return false;
TestEqual(TEXT("retirement releases canonical charge"),Queue->MetadataBytesForTest(),int64(0));
Restored.Connectivity->ReportOffline();
const auto Again=MakeShared<bool,ESPMode::ThreadSafe>(false);
Restored.Client.Put<void>(TEXT("/again")).NoAuth().QueueWhenOffline().OnQueued([Again](const FString&){*Again=true;}).Send();
if (!TestTrue(TEXT("released capacity reusable"),PumpUntil([Again]{return *Again;}))) return false;
return TestTrue(TEXT("final drain"),Restored.Drain());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionRejectedAdmissionCopiesNothingTest,"VaRest.Http.PersistenceRegression.RejectedAdmissionCopiesNothing", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionRejectedAdmissionCopiesNothingTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
for (int32 Scenario=0;Scenario<3;++Scenario)
{
    FScopedPersistenceTest Disk; FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer); FScopedPersistenceHooks Hooks(Disk.Root);
    const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
    if (Scenario==2) Hooks.Hooks->BeforeQueueLoad=Gate;
    FVaRestOfflinePolicy Policy; Policy.MaxQueued=1; Policy.MaxItemBytes=16; Policy.MaxBodyBytes=16;
    Rig.Connectivity->ReportOffline();
    Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),Disk.Root).WithOfflineQueue(TEXT("copies"),Policy);
    if (Scenario!=2 && !TestTrue(TEXT("loaded"),Rig.Drain())) return false;
    if (Scenario==1)
    {
        const auto Seed=MakeShared<bool,ESPMode::ThreadSafe>(false);
        Rig.Client.Put<void>(TEXT("/seed")).Body(TEXT("small")).NoAuth().QueueWhenOffline()
            .OnQueued([Seed](const FString&){*Seed=true;}).Send();
        if (!TestTrue(TEXT("full queue seeded"),PumpUntil([Seed]{return *Seed;}))) return false;
        if (!TestTrue(TEXT("seed drained"),Rig.Drain())) return false;
    }
    const int64 Before=Testing::Counters().QueueAdmissionBytesCopied.load();
    TArray<uint8> Bytes; Bytes.Init('x',Scenario==1 ? 8 : 1024*1024);
    auto Reply=MakeOutcome<void>();
    Observe(Rig.Client.Put<void>(TEXT("/rejected")).Body(MoveTemp(Bytes)).NoAuth().QueueWhenOffline(),Reply).Send();
    if (!TestTrue(TEXT("rejected asynchronously"),Rig.Wait(Reply))) return false;
    TestTrue(TEXT("bounded refusal"),Reply->Error.Kind==EVaRestErrorKind::Config);
    TestEqual(TEXT("zero extra admission payload copy"),Testing::Counters().QueueAdmissionBytesCopied.load(),Before);
    TestEqual(TEXT("no replay wire"),Rig.Fake->NumCalls(),0);
    Gate->Released=true;
    if (!TestTrue(TEXT("drained after refusal"),Rig.Drain())) return false;
    TestEqual(TEXT("no stranded admission"),Rig.Client.GetQueuedCount(),Scenario==1 ? 1 : 0);
}
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionRecoveredKnownSecretTest,"VaRest.Http.PersistenceRegression.RecoveredKnownSecret", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionRecoveredKnownSecretTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
const FString CacheRoot=FPaths::Combine(Disk.Root,TEXT("cache")),QueueRoot=FPaths::Combine(Disk.Root,TEXT("queue"));
{
    FTestRig Original; Original.Connectivity->ReportOffline();
    Original.Client.WithBaseUrl(TEXT("https://api.test")).WithStorageDirectories(CacheRoot,QueueRoot).WithOfflineQueue(TEXT("restored"));
    if (!TestTrue(TEXT("original load"),Original.Drain())) return false;
    const auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
    Original.Client.Put<void>(TEXT("/items")).Body(TEXT("q7!")).NoAuth().QueueWhenOffline()
        .OnQueued([Queued](const FString&){ ++*Queued; }).Send();
    if (!TestTrue(TEXT("original durable acknowledgement"),PumpUntil([Queued]{ return *Queued==1; }))) return false;
    if (!TestTrue(TEXT("original drained"),Original.Drain())) return false;
}
FLogCapture Logs; FTestRig Restored; FScopedTraceRecorder Trace(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies);
Restored.Client.WithBaseUrl(TEXT("https://api.test")).WithStorageDirectories(CacheRoot,QueueRoot).WithLogLevel(EVaRestLogLevel::Body);
Restored.Client.SetSecretValue(TEXT("late-known"),TEXT("q7!"));
const auto Event=MakeShared<TOptional<FVaRestQueuedResult>,ESPMode::ThreadSafe>();
Restored.Client.OnQueuedResult().AddLambda([Event](const FVaRestQueuedResult& Value){ *Event=Value; });
Restored.Client.WithOfflineQueue(TEXT("restored"));
if (!TestTrue(TEXT("restored item classified"),PumpUntil([Event]{ return Event->IsSet(); }))) return false;
if (!TestTrue(TEXT("all restored work drained"),Restored.Drain())) return false;
TestFalse(TEXT("known secret not replayed"),Event->GetValue().bSuccess);
TestTrue(TEXT("guard refusal is Config"),Event->GetValue().Error.Kind==EVaRestErrorKind::Config);
TestEqual(TEXT("no wire before full guard"),Restored.Fake->NumCalls(),0);
TestFalse(TEXT("callback error has no payload"),Event->GetValue().Error.Message.Contains(TEXT("q7!")));
FlushRedactionPipe(); TestFalse(TEXT("body logs redacted"),Logs.Contains(TEXT("q7!")));
const uint64 ReplayId=Event->GetValue().Response.RequestId;
const auto Events=Trace.Recorder->For(ReplayId);
if (!TestTrue(TEXT("replay metadata present"),Events.Num()>=2)) return false;
if (!TestTrue(TEXT("Started remains first"),Events[0].Kind==EVaRestTraceKind::Started)) return false;
TestFalse(TEXT("no request excerpt promised"),Events[0].Payload.Get<FVaRestTraceStarted>().bExcerptPending);
TestEqual(TEXT("refused replay announces no attempt"),Trace.Recorder->KindsOf(ReplayId),FString(TEXT("Started,Completed")));
for (const auto& Excerpt : Trace.Recorder->ExcerptsOf(ReplayId))
{
    TestTrue(TEXT("no replay request excerpt"),Excerpt.Owner!=EVaRestTraceExcerptOwner::Request);
    TestFalse(TEXT("no short secret disclosure"),Excerpt.Excerpt.ToText().Contains(TEXT("q7!")));
}
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionQueueAnchorTest,"VaRest.Http.PersistenceRegression.QueueAnchor", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionQueueAnchorTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
for (int32 Mutation=0;Mutation<3;++Mutation)
for (int32 Hold=0;Hold<3;++Hold)
{
    FScopedPersistenceTest Disk; FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer); FScopedPersistenceHooks Hooks(Disk.Root);
    const auto Barrier=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
    if (Hold==1) Hooks.Hooks->BeforeQueueLoad=Barrier;
    Rig.Client.WithBaseUrl(TEXT("https://api.test")).WithBearer(TEXT("A-live-token"))
        .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
        .WithOfflineQueue(TEXT("anchor")).WithMaxConcurrent(1);
    Rig.Client.SetSessionHeader(TEXT("X-Tenant"),TEXT("tenant-A-long-label"));
    if (Hold==1) Rig.Connectivity->ReportOffline();
    else if (!TestTrue(TEXT("load drained"),Rig.Drain())) return false;
    TSharedPtr<TTestOutcome<void>,ESPMode::ThreadSafe> Blocker;
    if (Hold==2)
    {
        Rig.Fake->Enqueue(FVaRestFakeResponse::Manual()); Blocker=MakeOutcome<void>();
        Observe(Rig.Client.Get<void>(TEXT("/blocker")).NoRetry(),Blocker.ToSharedRef()).Send();
        if (!TestEqual(TEXT("limiter occupied"),Rig.Fake->NumCalls(),1)) return false;
    }
    if (Hold==0) Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
    const auto Reply=MakeOutcome<void>(); const auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
    Observe(Rig.Client.Put<void>(TEXT("/operation")).Body(TEXT("safe-body")).NoRetry().QueueWhenOffline(),Reply).OnQueued([Queued](const FString&){ ++*Queued; }).Send();
    if (Hold==1 && !TestTrue(TEXT("initial load held"),PumpUntil([Barrier]{ return Barrier->Entered.load(); }))) return false;
    if (Hold==0)
    {
        if (!TestEqual(TEXT("A original dispatched"),Rig.Fake->NumCalls(),1)) return false;
        TestEqual(TEXT("A credentials on original"),Rig.Fake->GetCall(0).Request.Headers.Get(TEXT("Authorization")),FString(TEXT("Bearer A-live-token")));
    }
    if (Mutation==0) Rig.Client.SetBearerToken(TEXT("B-live-token"));
    if (Mutation==1) Rig.Client.SetSessionHeader(TEXT("X-Tenant"),TEXT("tenant-B-long-label"));
    if (Mutation==2) Rig.Client.SetBearerToken(TEXT("A-refreshed-token"),true);
    if (Hold==0) Rig.Fake->Complete(0,FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection));
    if (Hold==1) Barrier->Released=true;
    if (Hold==2) Rig.Fake->Complete(0,FVaRestFakeResponse::Text(200,TEXT("")));
    if (!TestTrue(TEXT("held operation settles"),PumpUntil([Reply,Queued]{ return Reply->CompleteCalls.load()+*Queued==1; }))) return false;
    if (Mutation<2)
    {
        TestTrue(TEXT("replacement rejects original identity"),Reply->Error.Kind==EVaRestErrorKind::Auth);
        TestEqual(TEXT("no B wire"),Rig.Fake->NumCalls(),Hold==1 ? 0 : 1);
        TestEqual(TEXT("no relabeled operation"),Rig.Client.GetQueuedCount(),0);
    }
    else if (Hold<2)
    { TestEqual(TEXT("same-identity refresh may queue"),*Queued,1); TestEqual(TEXT("one item"),Rig.Client.GetQueuedCount(),1); }
    else
    {
        TestEqual(TEXT("same-identity refresh may dispatch"),Reply->SuccessCalls.load(),1);
        if (!TestEqual(TEXT("one new dispatch"),Rig.Fake->NumCalls(),2)) return false;
        TestEqual(TEXT("live refreshed credentials"),Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("Authorization")),FString(TEXT("Bearer A-refreshed-token")));
    }
    if (!TestTrue(TEXT("holds drained"),Rig.Drain())) return false;
}
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionQueueBodyAfterNoRetryTest,"VaRest.Http.PersistenceRegression.QueueBodyAfterNoRetry", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionQueueBodyAfterNoRetryTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk; FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
Rig.Client.WithBaseUrl(TEXT("https://api.test"))
    .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
    .WithOfflineQueue(TEXT("body"));
if (!TestTrue(TEXT("load drained"),Rig.Drain())) return false;
Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection));
const auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
Rig.Client.Put<void>(TEXT("/items")).Body(TEXT("exact-payload")).NoAuth().NoRetry().QueueWhenOffline()
    .OnQueued([Queued](const FString&){ ++*Queued; }).Send();
if (!TestTrue(TEXT("queue ack"),PumpUntil([Queued]{ return *Queued==1; }))) return false;
if (!TestEqual(TEXT("one failed physical attempt"),Rig.Fake->NumCalls(),1)) return false;
const TArray<uint8> Physical=Rig.Fake->ReadBody(0);
TestTrue(TEXT("original body nonempty"),!Physical.IsEmpty());
const auto Loaded=MakeShared<FQueueLoad,ESPMode::ThreadSafe>();
FQueueStore::Load(QueueDirectory(FPaths::Combine(Disk.Root,TEXT("Queue")),TEXT("body")),TEXT("https://api.test"),
    FVaRestOfflinePolicy(),[Loaded](FQueueLoad&& Value){ *Loaded=MoveTemp(Value); });
if (!TestTrue(TEXT("record read"),Disk.Drain())) return false;
if (!TestEqual(TEXT("one durable item"),Loaded->Items.Num(),1)) return false;
TestTrue(TEXT("durable bytes exactly original"),Loaded->Items[0].Body.Bytes==Physical);
TestTrue(TEXT("NoAuth persisted"),Loaded->Items[0].AuthMode==EVaRestAuthMode::None);
Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT(""))); Rig.Connectivity->ReportOnline();
if (!TestTrue(TEXT("replay drained"),Rig.Drain())) return false;
if (!TestEqual(TEXT("one replay"),Rig.Fake->NumCalls(),2)) return false;
TestTrue(TEXT("replay bytes exactly original"),Rig.Fake->ReadBody(1)==Physical);
TestFalse(TEXT("anonymous replay"),Rig.Fake->GetCall(1).Request.Headers.Contains(TEXT("Authorization")));
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionClearConfiguredNamespaceTest,"VaRest.Http.PersistenceRegression.ClearConfiguredNamespace", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionClearConfiguredNamespaceTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http::Testing;
for (bool bPreviouslyTouchedA : {false,true})
{
    FScopedPersistenceTest Disk;
    const FString CacheA=FPaths::Combine(Disk.Root,TEXT("A")),CacheB=FPaths::Combine(Disk.Root,TEXT("B"));
    auto Policy=FVaRestCachePolicy::MemoryAndDisk(); Policy.MemoryBytes=0; Policy.DefaultTtlSeconds=600;
    {
        FTestRig Seed;
        Seed.Client.WithBaseUrl(TEXT("https://api.test")).WithStorageDirectories(CacheB,FPaths::Combine(Disk.Root,TEXT("Q"))).WithCache(Policy);
        Seed.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("old-B")));
        auto Reply=MakeOutcome<FString>(); Observe(Seed.Client.Get<FString>(TEXT("/entry")),Reply).Send();
        if (!TestTrue(TEXT("seed request"),Seed.Wait(Reply)) || !TestTrue(TEXT("seed disk"),Seed.Drain())) return false;
    }
    FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
    Rig.Client.WithBaseUrl(TEXT("https://api.test")).WithStorageDirectories(bPreviouslyTouchedA ? CacheA : CacheB,
        FPaths::Combine(Disk.Root,TEXT("Q"))).WithCache(Policy);
    if (bPreviouslyTouchedA)
    {
        Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("old-A")));
        auto Reply=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/entry")),Reply).Send();
        if (!TestTrue(TEXT("touch A"),Rig.Wait(Reply)) || !TestTrue(TEXT("A disk"),Rig.Drain())) return false;
        Rig.Client.WithStorageDirectories(CacheB,FPaths::Combine(Disk.Root,TEXT("Q")));
    }
    Rig.Client.ClearCache(); // B has never been looked up by this client
    if (!TestTrue(TEXT("configured clear applied"),Rig.Drain())) return false;
    const int32 Before=Rig.Fake->NumCalls();
    Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("new-B")));
    auto Reply=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/entry")),Reply).Send();
    if (!TestTrue(TEXT("new request"),Rig.Wait(Reply))) return false;
    TestEqual(TEXT("B requires wire"),Rig.Fake->NumCalls(),Before+1);
    TestFalse(TEXT("B old bytes unavailable"),Reply->Response.bFromCache);
    if (!TestTrue(TEXT("value present"),Reply->Value.IsSet())) return false;
    TestEqual(TEXT("new value"),Reply->Value.GetValue(),FString(TEXT("new-B")));
    if (!TestTrue(TEXT("B writes drained"),Rig.Drain())) return false;
    if (bPreviouslyTouchedA)
    {
        Rig.Client.WithStorageDirectories(CacheA,FPaths::Combine(Disk.Root,TEXT("Q")));
        auto A=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/entry")),A).Send();
        if (!TestTrue(TEXT("A lookup"),Rig.Wait(A))) return false;
        TestTrue(TEXT("unrelated A namespace preserved"),A->Response.bFromCache);
        if (!TestTrue(TEXT("A value"),A->Value.IsSet())) return false;
        TestEqual(TEXT("A bytes preserved"),A->Value.GetValue(),FString(TEXT("old-A")));
    }
    if (!TestTrue(TEXT("final drain"),Rig.Drain())) return false;
}
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionMandatoryCacheControlsTest,"VaRest.Http.PersistenceRegression.MandatoryCacheControls", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionMandatoryCacheControlsTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
using namespace VaRest::Http::Testing::PersistenceRegression;
for (int32 Operation=0;Operation<3;++Operation) // clear, no-store Remove, smaller capacity
{
    FScopedPersistenceTest Disk; FScopedCacheControls Controls;
    const FString Root=FPaths::Combine(Disk.Root,TEXT("store"));
    const FCacheEntry Entry=SafeEntry();
    TSharedPtr<FDiskCache,ESPMode::ThreadSafe> Store=FDiskCache::Open(Root,1024*1024);
    Store->Put(Entry,FPersistenceGuard());
    if (!TestTrue(TEXT("seed store"),Disk.Drain())) return false;
    const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
    ON_SCOPE_EXIT { Gate->Released=true; TestTrue(TEXT("mandatory cache controls cleanup drained"),Disk.Drain()); };
    LaunchPersistence([Gate]{ WaitPersistenceBarrier(Gate); FIoResult Result; Result.bOk=true; return Result; },[](FIoResult&&){});
    for (int32 Index=1;Index<8192;++Index)
        if (!TestTrue(TEXT("fill real slots"),LaunchPersistence([]{ FIoResult Result; Result.bOk=true; return Result; },[](FIoResult&&){})))
        { Gate->Released=true; return false; }
    if (Operation==0) Store->Clear();
    if (Operation==1) Store->Remove(Entry.Key);
    if (Operation==2) Store=FDiskCache::Open(Root,1);
    TestTrue(TEXT("refused mandatory operation quarantined"),Store->IsQuarantined());
    Store.Reset(); Store=FDiskCache::Open(Root,1024*1024);
    TestTrue(TEXT("release/reopen retains intent"),Store->IsQuarantined());
    const auto Hit=MakeShared<bool,ESPMode::ThreadSafe>(true);
    Store->Find(Entry.Key,0,FDateTime::UtcNow(),[Hit](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Value){ *Hit=Value.IsValid(); });
    TestFalse(TEXT("refused control cannot expose prior bytes"),*Hit);
    Store->Put(Entry,FPersistenceGuard());
    Gate->Released=true;
    if (!TestTrue(TEXT("saturation drained"),Disk.Drain())) return false;
    {
        FScopedPersistenceHooks Hooks(Disk.Root); Hooks.Hooks->bFailCacheControl=true;
        Store=FDiskCache::Open(Root,1024*1024); // retry fails actual IO result
        if (!TestTrue(TEXT("failed maintenance drained"),Disk.Drain())) return false;
        TestTrue(TEXT("IO failure remains quarantined"),Store->IsQuarantined());
    }
    Store.Reset(); Store=FDiskCache::Open(Root,1024*1024);
    if (!TestTrue(TEXT("later successful retry"),Disk.Drain())) return false;
    TestFalse(TEXT("successful control lifts quarantine"),Store->IsQuarantined());
    Store->Find(Entry.Key,0,FDateTime::UtcNow(),[Hit](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Value){ *Hit=Value.IsValid(); });
    if (!TestTrue(TEXT("postcontrol lookup"),Disk.Drain())) return false;
    TestFalse(TEXT("invalidated bytes never resurrect"),*Hit);
}
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionCapacityReadChargeTest,"VaRest.Http.PersistenceRegression.CapacityReadCharge", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionCapacityReadChargeTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
using namespace VaRest::Http::Testing::PersistenceRegression;
FScopedPersistenceTest Disk;
const FString Root=FPaths::Combine(Disk.Root,TEXT("store"));
auto Store=FDiskCache::Open(Root,1024*1024); auto Entry=SafeEntry(); Entry.Body.Init(71,128*1024);
Store->Put(Entry,FPersistenceGuard());
if (!TestTrue(TEXT("seed persisted"),Disk.Drain())) return false;
FScopedPersistenceHooks Hooks(Disk.Root); auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->BeforeRead=Gate;
auto OldHit=MakeShared<bool,ESPMode::ThreadSafe>(true),NewHit=MakeShared<bool,ESPMode::ThreadSafe>(true);
Store->Find(Entry.Key,0,FDateTime::UtcNow(),[OldHit](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Value){ *OldHit=Value.IsValid(); });
if (!TestTrue(TEXT("old read held"),PumpUntil([Gate]{ return Gate->Entered.load(); }))) return false;
FDiskCache::Open(Root,1);
Store->Find(Entry.Key,0,FDateTime::UtcNow(),[NewHit](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Value){ *NewHit=Value.IsValid(); });
TestEqual(TEXT("old and new immutable read caps remain charged"),PersistencePendingBytesForTest(),int64(8*1024*1024+16*1024*1024+16*1024*1024+8+16*1024*1024));
Gate->Released=true;
if (!TestTrue(TEXT("serialized reads/control drained"),Disk.Drain())) return false;
TestFalse(TEXT("old epoch discarded"),*OldHit); TestFalse(TEXT("new small-cap read cannot reuse old allowance"),*NewHit);
TestEqual(TEXT("all resident DTO charge released"),PersistencePendingBytesForTest(),int64(0));
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionControlCapFallbackTest,"VaRest.Http.PersistenceRegression.ControlCapFallback", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionControlCapFallbackTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
using namespace VaRest::Http::Testing::PersistenceRegression;
FScopedPersistenceTest Disk;
FScopedCacheControls Controls;
const FString Original=FPaths::Combine(Disk.Root,TEXT("original"));
auto Store=FDiskCache::Open(Original,1024*1024); const auto Entry=SafeEntry(); Store->Put(Entry,FPersistenceGuard());
if (!TestTrue(TEXT("baseline persisted"),Disk.Drain())) return false;
const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
ON_SCOPE_EXIT { Gate->Released=true; TestTrue(TEXT("control cap fallback cleanup drained"),Disk.Drain()); };
LaunchPersistence([Gate]{ WaitPersistenceBarrier(Gate); return FIoResult(); },[](FIoResult&&){});
for (int32 Index=0;Index<4097;++Index)
    FDiskCache::Open(FPaths::Combine(Disk.Root,FString::Printf(TEXT("pending-%d"),Index)),1024)->Clear();
TestTrue(TEXT("cap disables even previously healthy namespace"),Store->IsQuarantined());
Store->Put(Entry,FPersistenceGuard());
Gate->Released=true;
if (!TestTrue(TEXT("already admitted controls drain"),Disk.Drain())) return false;
// The permanent process fallback survives successful old controls. Read only after their
// workspace charges release, so a removed fallback cannot hide behind worker admission refusal.
const auto Hit=MakeShared<bool,ESPMode::ThreadSafe>(false);
const auto Body=MakeShared<TArray<uint8>,ESPMode::ThreadSafe>();
const auto Answered=MakeShared<int32,ESPMode::ThreadSafe>(0);
Store->Find(Entry.Key,0,FDateTime::UtcNow(),[Hit,Body,Answered](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Value)
{*Hit=Value.IsValid();if (Value) *Body=Value->Body;++*Answered;});
if (!TestTrue(TEXT("actual fallback read drains"),Disk.Drain())) return false;
TestEqual(TEXT("actual fallback read callback once"),*Answered,1);
TestFalse(TEXT("process fallback cannot return actual old bytes"),*Hit);
TestFalse(TEXT("seeded old body never returned"),*Body==Entry.Body);
TestTrue(TEXT("fallback does not reset when earlier controls succeed"),FDiskCache::Open(Original,1024*1024)->IsQuarantined());
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionPhysicalReachabilityOnlyTest,"VaRest.Http.PersistenceRegression.PhysicalReachabilityOnly", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionPhysicalReachabilityOnlyTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
using namespace VaRest::Http::Testing::PersistenceRegression;
for (bool bDelayed : {false,true})
{
    FScopedPersistenceTest Disk; FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
    Rig.Client.WithBaseUrl(TEXT("https://api.test"))
        .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),FPaths::Combine(Disk.Root,TEXT("queue")))
        .WithOfflineQueue(TEXT("reachability"));
    const auto Stage=MakeShared<FLogicalReplayFailure,ESPMode::ThreadSafe>(); Stage->bHold=bDelayed;
    Rig.Client.Use(Stage,EVaRestStageSlot::Outer); Rig.Connectivity->ReportOffline();
    if (!TestTrue(TEXT("initial load"),Rig.Drain())) return false;
    const auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
    Rig.Client.Put<void>(TEXT("/items")).Body(TEXT("safe")).QueueWhenOffline()
        .OnQueued([Queued](const FString&){ ++*Queued; }).Send();
    if (!TestTrue(TEXT("durable item"),PumpUntil([Queued]{ return *Queued==1; }))) return false;
    Rig.Connectivity->ReportOnline();
    if (!TestTrue(TEXT("replay entered"),PumpUntil([Stage]{ return Stage->Entered==1; }))) return false;
    if (bDelayed)
    {
        Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
        auto Reach=MakeOutcome<void>(); Observe(Rig.Client.Get<void>(TEXT("/reachable")).NoRetry(),Reach).Send();
        if (!TestEqual(TEXT("real attempt dispatched"),Rig.Fake->NumCalls(),1)) return false;
        Rig.Connectivity->ReportOffline();
        Rig.Fake->Complete(0,FVaRestFakeResponse::Text(503,TEXT("")));
        if (!TestTrue(TEXT("physical503 processed"),Rig.Wait(Reach))) return false;
        TestTrue(TEXT("physical HTTP restored online"),Rig.Connectivity->IsOnline());
        Stage->bHold=false; Stage->Fail();
    }
    if (!TestTrue(TEXT("logical failure settled"),Rig.Drain())) return false;
    TestTrue(TEXT("logical Connection cannot overwrite reachability"),Rig.Connectivity->IsOnline());
    TestEqual(TEXT("head retained"),Rig.Client.GetQueuedCount(),1);
    TestEqual(TEXT("synthetic replay never sent"),Rig.Fake->NumCalls(),bDelayed ? 1 : 0);
    const auto Queue=FClientAccess::State(Rig.Client)->Queue;
    if (!TestTrue(TEXT("live queue for explicit retry edges"),Queue.IsValid())) return false;
    Queue->ScheduleReplay(); Queue->ScheduleReplay();
    if (!TestTrue(TEXT("repeated queue wake settled"),Rig.Drain())) return false;
    TestEqual(TEXT("no immediate retry loop"),Stage->Entered,1);
    Rig.Clock->Advance(9);
    if (!TestTrue(TEXT("before retry tick drained"),Rig.Drain())) return false;
    TestEqual(TEXT("bounded queue-local pause"),Stage->Entered,1);
    Rig.Clock->Advance(1);
    if (!TestTrue(TEXT("retry tick drained"),Rig.Drain())) return false;
    TestEqual(TEXT("one later local retry"),Stage->Entered,2);
}
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionDeadlineBeforeDiskHandoffTest,"VaRest.Http.PersistenceRegression.DeadlineBeforeDiskHandoff", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionDeadlineBeforeDiskHandoffTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
using namespace VaRest::Http::Testing::PersistenceRegression;
for (bool bStale : {false,true})
{
    FScopedPersistenceTest Disk; FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
    auto Clock=MakeShared<FDelayedTimerClock,ESPMode::ThreadSafe>();
    auto Policy=FVaRestCachePolicy::MemoryAndDisk(); Policy.MemoryBytes=0;
    Policy.DefaultTtlSeconds=bStale ? 0 : 600; Policy.bServeStaleOnError=true;
    Rig.Client.WithClock(Clock).WithBaseUrl(TEXT("https://api.test"))
        .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),FPaths::Combine(Disk.Root,TEXT("queue"))).WithCache(Policy);
    Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("cached")));
    auto Seed=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/entry")),Seed).Send();
    if (!TestTrue(TEXT("seed response"),Rig.Wait(Seed)) || !TestTrue(TEXT("seed IO"),Rig.Drain())) return false;
    FScopedPersistenceHooks Hooks(Disk.Root);
    auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->BeforeRead=Gate;
    if (bStale) Rig.Connectivity->ReportOffline();
    FVaRestRetryPolicy Deadline; Deadline.MaxRetries=0; Deadline.Deadline=1;
    auto Reply=MakeOutcome<FString>();
    Observe(Rig.Client.Get<FString>(TEXT("/entry")).Retry(Deadline),Reply).Send();
    if (!TestTrue(TEXT("read held"),PumpUntil([Gate]{ return Gate->Entered.load(); }))) return false;
    Clock->AdvanceNowOnly(2); // no timer dispatch: reproduces wall-clock/ticker lag
    Gate->Released=true;
    if (!TestTrue(TEXT("handback settles"),Rig.Wait(Reply))) return false;
    TestTrue(TEXT("deadline wins over fresh/stale bytes"),Reply->Error.Kind==EVaRestErrorKind::Timeout);
    TestEqual(TEXT("one completion"),Reply->CompleteCalls.load(),1);
    TestEqual(TEXT("no extra wire"),Rig.Fake->NumCalls(),1);
    if (!TestTrue(TEXT("deadline drain"),Rig.Drain())) return false;
    TestEqual(TEXT("wait timer disarmed"),Clock->Pending(),0);
}
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionExchangeShutdownTest,"VaRest.Http.PersistenceRegression.ExchangeShutdown", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionExchangeShutdownTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
for (int32 Hold=0;Hold<3;++Hold) // disk read, prepared uncommitted item, accepted rename
{
    FScopedPersistenceTest Disk; // destructor runs after scoped executor restores production admission
    FScopedPersistenceRuntime Runtime;
    FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer); FScopedPersistenceHooks Hooks(Disk.Root);
    auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
    if (Hold==0) Hooks.Hooks->BeforeRead=Gate;
    if (Hold==1) Hooks.Hooks->AfterPrepare=Gate;
    if (Hold==2) Hooks.Hooks->BeforeRename=Gate;
    auto Policy=FVaRestCachePolicy::MemoryAndDisk(); Policy.MemoryBytes=0;
    Rig.Client.WithBaseUrl(TEXT("https://api.test"))
        .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),FPaths::Combine(Disk.Root,TEXT("queue"))).WithCache(Policy);
    if (Hold!=0) { Rig.Client.WithOfflineQueue(TEXT("shutdown")); Rig.Connectivity->ReportOffline(); }
    if (!TestTrue(TEXT("setup drained"),Rig.Drain())) return false;
    auto Reply=MakeOutcome<void>(); const auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
    const auto Handle=[&]
    {
        if (Hold==0) return Observe(Rig.Client.Get<void>(TEXT("/entry")),Reply).Send();
        return Observe(Rig.Client.Put<void>(TEXT("/operation")).Body(TEXT("safe")).QueueWhenOffline(),Reply)
            .OnQueued([Queued](const FString&){ ++*Queued; }).Send();
    }();
    if (!TestTrue(TEXT("exchange at owned hold"),PumpUntil([Gate]{ return Gate->Entered.load(); }))) return false;
    const auto State=FClientAccess::State(Rig.Client);
    if (!TestTrue(TEXT("state retained"),State.IsValid())) return false;
    const auto Unrelated=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    UE::Tasks::Launch(TEXT("VaRest.UnrelatedExchangeShutdown"),[Unrelated]{ Unrelated->store(true); },
        UE::Tasks::ETaskPriority::Normal,UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
    State->CancelAll(NAME_None,EVaRestCancelReason::Shutdown); // same per-client path CancelForShutdown uses
    if (State->Queue.IsValid()) State->Queue->StopForShutdown();
    Gate->Released=true;
    Runtime.Stop(); // drains only owned results, including the accepted commit acknowledgement
    TestFalse(TEXT("no unrelated GT pump"),Unrelated->load());
    TestEqual(TEXT("no deferred global Complete wrapper"),State->PendingApiCompletionWrappers,0);
    TestEqual(TEXT("all owned exchange accounting settled"),State->NumInFlight(),0);
    TestTrue(TEXT("handle done after actual IO for commit"),Capture->IsDone());
    TestEqual(TEXT("ordinary callback suppressed"),Reply->CompleteCalls.load(),0);
    TestEqual(TEXT("queued callback suppressed"),*Queued,0);
    TestTrue(TEXT("owned slots and DTO bytes released"),PersistenceIdle());
    if (State->Queue.IsValid())
    {
        TestEqual(TEXT("all admission tickets settled during stop"),State->Queue->NumAdmissionsForTest(),0);
        TestEqual(TEXT("committed shutdown item accounted"),State->Queue->Count(),Hold==2?1:0);
    }
    // Only after all Stop assertions may normal automation pump its witness.
    if (!TestTrue(TEXT("witness cleanup"),PumpUntil([Unrelated]{ return Unrelated->load(); }))) return false;
}
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionAcceptedCompletionShutdownTest,"VaRest.Http.PersistenceRegression.AcceptedCompletionShutdown", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionAcceptedCompletionShutdownTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
FScopedPersistenceRuntime Runtime;
TWeakPtr<FClientState,ESPMode::ThreadSafe> WeakState;
const auto Unrelated=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
{
    FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer); Rig.Connectivity->ReportOffline();
    Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),Disk.Root).WithOfflineQueue(TEXT("accepted-stop"));
    if (!TestTrue(TEXT("queue loaded"),Rig.Drain())) return false;
    const auto State=FClientAccess::State(Rig.Client); WeakState=State;
    const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
    ON_SCOPE_EXIT { Gate->Released=true; };
    if (!TestTrue(TEXT("blocking job admitted"),LaunchPersistence([Gate]{WaitPersistenceBarrier(Gate);return FIoResult();},[](FIoResult&&){}))) return false;
    for (int32 Index=1;Index<8192;++Index)
        if (!TestTrue(TEXT("real callback slot filled"),LaunchPersistence([]{return FIoResult();},[](FIoResult&&){}))) return false;
    auto Reply=MakeOutcome<void>();
    const auto Handle=Observe(Rig.Client.Put<void>(TEXT("/operation")).Body(TEXT("safe")).NoAuth().QueueWhenOffline(),Reply).Send();
    TestEqual(TEXT("accepted failure remains non-inline"),Reply->CompleteCalls.load(),0);
    TestEqual(TEXT("no global wrapper even before Stop"),State->PendingApiCompletionWrappers,0);
    TestEqual(TEXT("exchange pending owned completion"),State->NumInFlight(),1);
    UE::Tasks::Launch(TEXT("VaRest.AcceptedStopWitness"),[Unrelated]{Unrelated->store(true);},
        UE::Tasks::ETaskPriority::Normal,UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
    // No PumpUntil, TaskGraph or core ticker between Send and Stop.
    State->CancelAll(NAME_None,EVaRestCancelReason::Shutdown);
    State->Queue->StopForShutdown(); Gate->Released=true;
    Runtime.Stop();
    TestFalse(TEXT("unrelated GT witness untouched"),Unrelated->load());
    TestEqual(TEXT("zero global wrappers after Stop"),State->PendingApiCompletionWrappers,0);
    TestEqual(TEXT("zero in-flight exchanges"),State->NumInFlight(),0);
    TestEqual(TEXT("no user delivery"),Reply->CompleteCalls.load(),0);
    TestTrue(TEXT("handle silently terminal"),Capture->IsDone());
    TestTrue(TEXT("owned completion and IO slots released"),PersistenceIdle());
}
TestFalse(TEXT("no completion-owned client retention"),WeakState.IsValid());
TestFalse(TEXT("still no unrelated pump"),Unrelated->load());
return TestTrue(TEXT("witness cleanup after assertions"),PumpUntil([Unrelated]{return Unrelated->load();}));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionCompletionPermitRefusalTest,"VaRest.Http.PersistenceRegression.CompletionPermitRefusal", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionCompletionPermitRefusalTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk; FScopedPersistenceRuntime Runtime; FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
Rig.Connectivity->ReportOffline();
Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),Disk.Root).WithOfflineQueue(TEXT("permit-cap"));
if (!TestTrue(TEXT("queue loaded"),Rig.Drain())) return false;
TArray<uint64> Permits;
ON_SCOPE_EXIT { for (uint64 Ticket : Permits) ReleasePersistenceCompletion(Ticket); };
for (int32 Index=0;Index<8192;++Index)
{
    const uint64 Ticket=AcquirePersistenceCompletion();
    if (!TestTrue(TEXT("real bounded permit"),Ticket!=0)) return false;
    Permits.Add(Ticket);
}
TestEqual(TEXT("no unbounded completion lane"),AcquirePersistenceCompletion(),uint64(0));
const uint64 Before=PersistenceCounters().Launched.load();
const int64 Copied=Testing::Counters().QueueAdmissionBytesCopied.load();
auto Reply=MakeOutcome<void>();
Observe(Rig.Client.Put<void>(TEXT("/operation")).Body(TEXT("safe")).NoAuth().QueueWhenOffline(),Reply).Send();
TestEqual(TEXT("ordinary refusal not inline"),Reply->CompleteCalls.load(),0);
TestEqual(TEXT("no persistence job on refused permit"),PersistenceCounters().Launched.load(),Before);
TestEqual(TEXT("no payload ownership"),Testing::Counters().QueueAdmissionBytesCopied.load(),Copied);
TestEqual(TEXT("no queue reservation"),Rig.Client.GetQueuedCount(),0);
TestEqual(TEXT("ordinary completion wrapper is explicit exception"),FClientAccess::State(Rig.Client)->PendingApiCompletionWrappers,1);
for (uint64 Ticket : Permits) ReleasePersistenceCompletion(Ticket);
Permits.Reset();
if (!TestTrue(TEXT("ordinary refusal delivered later"),Rig.Wait(Reply))) return false;
TestTrue(TEXT("Config refusal"),Reply->Error.Kind==EVaRestErrorKind::Config);
TestEqual(TEXT("one completion"),Reply->CompleteCalls.load(),1);
const uint64 Reused=AcquirePersistenceCompletion();
TestTrue(TEXT("released permits reusable"),Reused!=0); ReleasePersistenceCompletion(Reused);
if (!TestTrue(TEXT("ordinary wrapper drained"),Rig.Drain())) return false;
TestEqual(TEXT("no wrapper remains"),FClientAccess::State(Rig.Client)->PendingApiCompletionWrappers,0);
Runtime.Stop(); return TestTrue(TEXT("all owned accounts empty"),PersistenceIdle());
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionFlushFallbackNotInlineTest,"VaRest.Http.PersistenceRegression.FlushFallbackNotInline", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionFlushFallbackNotInlineTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk; FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
Rig.Client.WithBaseUrl(TEXT("https://api.test"))
    .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),FPaths::Combine(Disk.Root,TEXT("queue")))
    .WithOfflineQueue(TEXT("flush-stack"));
Rig.Connectivity->ReportOffline();
if (!TestTrue(TEXT("queue loaded"),Rig.Drain())) return false;
const auto Depth=MakeShared<int32,ESPMode::ThreadSafe>(0);
const auto SeenDepth=MakeShared<int32,ESPMode::ThreadSafe>(-1);
const auto Result=MakeShared<TOptional<bool>,ESPMode::ThreadSafe>();
if (!TestTrue(TEXT("flush admitted with owned delivery slot"),Rig.Client.FlushQueue([Depth,SeenDepth,Result](bool bOk)
    { *SeenDepth=*Depth; *Result=bOk; }))) return false;
const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
ON_SCOPE_EXIT { Gate->Released=true; };
LaunchPersistence([Gate]{ WaitPersistenceBarrier(Gate); return FIoResult(); },[](FIoResult&&){});
for (int32 Index=1;Index<8192;++Index)
    if (!TestTrue(TEXT("saturate IO slots"),LaunchPersistence([]{ return FIoResult(); },[](FIoResult&&){})))
    { Gate->Released=true; return false; }
*Depth=1;
const auto Original=MakeOutcome<void>();
Observe(Rig.Client.Put<void>(TEXT("/items")).Body(TEXT("safe")).QueueWhenOffline(),Original).Send();
TestFalse(TEXT("finish-admission refusal did not call another user's flush"),Result->IsSet());
*Depth=0;
PumpPersistence(); // retained poll observes barrier refusal, stores false, does not deliver inline
TestFalse(TEXT("first readiness poll retains result slot"),Result->IsSet());
PumpPersistence();
if (!TestTrue(TEXT("next owned pump delivers refusal"),Result->IsSet())) { Gate->Released=true; return false; }
TestFalse(TEXT("truthful failed barrier"),Result->GetValue());
TestEqual(TEXT("delivery is off Send stack"),*SeenDepth,0);
Gate->Released=true;
if (!TestTrue(TEXT("cleanup drained"),Rig.Drain())) return false;
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionFlushContinuationBoundTest,"VaRest.Http.PersistenceRegression.FlushContinuationBound", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionFlushContinuationBoundTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
FScopedPersistenceRuntime Runtime;
FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);
Rig.Client.WithBaseUrl(TEXT("https://api.test"))
    .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),FPaths::Combine(Disk.Root,TEXT("queue")))
    .WithOfflineQueue(TEXT("continuation-bound"));
if (!TestTrue(TEXT("queue loaded"),Rig.Drain())) return false;
for (int32 Index=0;Index<8192;++Index)
    if (!TestTrue(TEXT("owned poll slot admitted"),PostPersistencePoll([]{return false;}))) return false;
const auto Calls=MakeShared<int32,ESPMode::ThreadSafe>(0);
TestFalse(TEXT("flush admission refuses full continuation queue"),Rig.Client.FlushQueue([Calls](bool){ ++*Calls; }));
const auto State=FClientAccess::State(Rig.Client);
if (!TestTrue(TEXT("registered state"),State.IsValid() && State->Queue.IsValid())) return false;
TestEqual(TEXT("no stranded waiter"),State->Queue->NumFlushWaitersForTest(),0);
State->Queue->StopForShutdown(); Runtime.Stop();
TestEqual(TEXT("false admission has no later callback"),*Calls,0);
TestTrue(TEXT("all reserved delivery slots discarded"),PersistenceIdle());
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionClosedCacheHandbackTest,"VaRest.Http.PersistenceRegression.ClosedCacheHandback", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionClosedCacheHandbackTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
using namespace VaRest::Http::Testing::PersistenceRegression;
FScopedPersistenceTest Disk; FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer); FScopedTraceRecorder Trace;
Rig.Client.WithCache(FVaRestCachePolicy::MemoryAndDisk())
    .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),FPaths::Combine(Disk.Root,TEXT("queue")));
const auto Outer=MakeShared<FEarlyOuter,ESPMode::ThreadSafe>(); Rig.Client.Use(Outer,EVaRestStageSlot::Outer);
Rig.Fake->bCompleteOnWorker=false; Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
const auto Reply=MakeOutcome<void>(); const auto Handle=Observe(Rig.Client.Get<void>(TEXT("/late")),Reply).Send();
if (!TestTrue(TEXT("disk miss dispatched"),PumpUntil([&Rig]{ return Rig.Fake->NumCalls()==1; }))) return false;
Rig.Fake->Complete(0,FVaRestFakeResponse::Text(200,TEXT("late cache bytes")));
// Transport's GT handback is queued. Answer through Outer before pumping it.
Outer->FinishNow();
if (!TestTrue(TEXT("outer answer completed"),Capture->IsDone())) return false;
const FString Before=Trace.Recorder->KindsOf(Handle.GetId());
if (!TestTrue(TEXT("late completion drained"),Rig.Drain())) return false;
TestEqual(TEXT("no metadata event after Completed"),Trace.Recorder->KindsOf(Handle.GetId()),Before);
TestEqual(TEXT("one client completion"),Reply->CompleteCalls.load(),1);
const auto State=FClientAccess::State(Rig.Client);
if (!TestTrue(TEXT("state exists"),State.IsValid())) return false;
TestEqual(TEXT("closed request did not resurrect memory entry"),State->GetMemoryCache().Num(),0);
TestEqual(TEXT("inner once callback forwarded or discarded once"),Outer->InnerCalls,1);
const auto Files=MakeShared<int32,ESPMode::ThreadSafe>(0);
LaunchPersistence([Root=Disk.Root]
{
    FIoResult Result; Result.bOk=true;
    FPlatformFileManager::Get().GetPlatformFile().IterateDirectoryRecursively(*Root,
        [&Result](const TCHAR* Path,bool bDirectory)
        { if (!bDirectory && FString(Path).EndsWith(TEXT(".json"))) Result.Names.Add(FString(Path)); return true; });
    return Result;
},[Files](FIoResult&& Result){ *Files=Result.Names.Num(); });
if (!TestTrue(TEXT("disk inspection"),Disk.Drain())) return false;
TestEqual(TEXT("closed request created no disk record"),*Files,0);
return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRegressionRetainedOwnersTest,"VaRest.Http.PersistenceRegression.RetainedOwners", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRegressionRetainedOwnersTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;using namespace VaRest::Http::Testing;using namespace VaRest::Http::Testing::PersistenceRegression;
    const int32 OffThreadBefore=Testing::Counters().ClientStatesDestroyedOffGameThread.load();
    const uint64 IoBefore=PersistenceCounters().OperationsOnGameThread.load();
    for (int32 Mode=0;Mode<4;++Mode) // cache cancel, prepare cancel, queued completion, early Outer over held Prepare
    {
        FScopedPersistenceTest Disk;FScopedCacheControls Controls;
        TWeakPtr<FClientState,ESPMode::ThreadSafe> WeakState;
        TSharedPtr<FVaRestVirtualClock,ESPMode::ThreadSafe> RetainedClock;
        {
            FTestRig Rig; const auto Capture=MakeShared<VaRest::Http::Testing::PersistenceRegression::FExchangeCapture,ESPMode::ThreadSafe>(); Rig.Client.Use(Capture,EVaRestStageSlot::Outer);RetainedClock=Rig.Clock;WeakState=FClientAccess::State(Rig.Client);
            FScopedPersistenceHooks Hooks(Disk.Root);auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
            ON_SCOPE_EXIT {Gate->Released=true;Rig.Client.CancelAll();Rig.Drain();};
            Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),Disk.Root);
            if (Mode==0) {auto Policy=FVaRestCachePolicy::MemoryAndDisk();Policy.MemoryBytes=0;Rig.Client.WithCache(Policy);Hooks.Hooks->BeforeRead=Gate;}
            else {Rig.Client.WithOfflineQueue(TEXT("owner"));Rig.Connectivity->ReportOffline();Hooks.Hooks->AfterPrepare=Gate;}
            if (!TestTrue(TEXT("owner fixture initialized"),Rig.Drain())) return false;
            auto Outer=MakeShared<FEarlyOuter,ESPMode::ThreadSafe>();if (Mode==3) Rig.Client.Use(Outer,EVaRestStageSlot::Outer);
            const auto Probe=MakeShared<FOutcomeProbe,ESPMode::ThreadSafe>();Rig.Client.Use(Probe,EVaRestStageSlot::Outer);
            auto Reply=MakeOutcome<void>();auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
            auto Request=Mode==0 ? Rig.Client.Get<void>(TEXT("/entry")) : Rig.Client.Put<void>(TEXT("/operation")).Body(TEXT("safe")).QueueWhenOffline();
            const auto Handle=Observe(MoveTemp(Request),Reply).OnQueued([Queued](const FString&){++*Queued;}).Send();
            if (!TestTrue(TEXT("owner worker hold"),PumpUntil([Gate]{return Gate->Entered.load();}))) return false;
            if (Mode==3)
            {
                Outer->FinishNow();
                TestEqual(TEXT("early Outer completes once before IO release"),Reply->CompleteCalls.load(),1);
                TestEqual(TEXT("held inner is interrupted before IO release"),Outer->InnerCalls,1);
                TestEqual(TEXT("probe sees one canceled inner chain"),Probe->Count(Handle.GetId(),EVaRestTransportFailure::Canceled),1);
            }
            else if (Mode<2) Handle.Cancel();
            Gate->Released=true;
            if (!TestTrue(TEXT("owner completion drained"),Rig.Drain())) return false;
            TestTrue(TEXT("handle terminal after drain"),Capture->IsDone());
            TestEqual(TEXT("no in-flight request"),FClientAccess::State(Rig.Client)->NumInFlight(),0);
            TestEqual(TEXT("typed completion count"),Reply->CompleteCalls.load(),Mode==2?0:1);
            TestEqual(TEXT("queued completion count"),*Queued,Mode==2?1:0);
            TestEqual(TEXT("outer inner outcome exactly once"),Probe->Total(Handle.GetId()),1);
        }
        TestFalse(TEXT("all public handles dropped releases client"),WeakState.IsValid());
        TestEqual(TEXT("no retained connectivity/request timers"),RetainedClock->NumPendingTimers(),0);
        if (!TestTrue(TEXT("post-owner IO drain"),Disk.Drain())) return false;
    }
    TestEqual(TEXT("client destructor stays on GT"),Testing::Counters().ClientStatesDestroyedOffGameThread.load(),OffThreadBefore);
    TestEqual(TEXT("no GT file operation"),PersistenceCounters().OperationsOnGameThread.load(),IoBefore);
    return true;
}

#endif
