// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS
#include "VaRestHttpTesting.h"
#include "VaRestQueueStore.h"
#include "VaRestClientState.h"
#include "VaRestIdentity.h"
#include "VaRestPersistence.h"
#include "VaRestAuth.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/Paths.h"
#if PLATFORM_LINUX
#include <sys/stat.h>
#include <unistd.h>
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestQueueStoreRoundTripAndRejectTest,"VaRest.Http.QueueStore.RoundTripAndReject", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestQueueStoreRoundTripAndRejectTest::RunTest(const FString& Parameters)
{
using namespace VaRest::Http;
FQueueItem Item;
Item.QueueId = FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);
Item.Sequence = 9007199254740993ULL; // greater than exact double integer range
Item.OriginalRun = QueueRunId();
Item.OriginalRequestId = 123;
Item.CreatedUtc = FDateTime(2026,10,23,12,0,0);
Item.Scope = HashFields(TEXT("test-scope"), TArray<FString>{TEXT("A")});
Item.Verb = TEXT("POST");
Item.Url = TEXT("https://api.test:443/items");
Item.Headers.Set(TEXT("Content-Type"), TEXT("application/octet-stream"));
Item.IdempotencyKey = TEXT("stable-operation-id");
Item.Headers.Set(TEXT("Idempotency-Key"), Item.IdempotencyKey);
Item.Body = FVaRestRequestBody::FromBytes(TArray<uint8>{0,255,1}, EVaRestBodyKind::Binary, TEXT("application/octet-stream"));
Item.Tag = TEXT("Upload");
Item.AuthMode = EVaRestAuthMode::None;
Item.bIdempotent = true;
const auto Encoded = EncodeQueueRecord(Item);
auto Decoded = DecodeQueueRecord(Encoded, TEXT("https://api.test:443"), FVaRestOfflinePolicy());
if (!TestTrue(TEXT("round trip"), Decoded.HasValue())) return false;
const FQueueItem& Read = Decoded.GetValue();
TestEqual(TEXT("uint64 sequence is exact"), Read.Sequence, Item.Sequence);
TestTrue(TEXT("binary bytes exact"), Read.Body.Bytes == Item.Body.Bytes);
TestTrue(TEXT("NoAuth preserved"), Read.AuthMode == EVaRestAuthMode::None);
TestEqual(TEXT("idempotency stable"), Read.IdempotencyKey, Item.IdempotencyKey);
TestTrue(TEXT("wrong origin refuses"), DecodeQueueRecord(Encoded, TEXT("https://other.test:443"), FVaRestOfflinePolicy()).HasError());
TArray<uint8> Truncated = Encoded;
Truncated.SetNum(Encoded.Num() / 2);
TestTrue(TEXT("truncated JSON refuses"), DecodeQueueRecord(Truncated, TEXT("https://api.test:443"), FVaRestOfflinePolicy()).HasError());
return true;
}

namespace VaRest::Http::QueueStoreTestPrivate
{
    FQueueItem Item()
    {
        FQueueItem Result;
        Result.QueueId=FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);
        Result.OriginalRun=QueueRunId(); Result.Sequence=1; Result.OriginalRequestId=12;
        Result.CreatedUtc=FDateTime::UtcNow(); Result.Scope=HashFields(TEXT("test"),TArray<FString>{TEXT("scope")});
        Result.Verb=TEXT("POST"); Result.Url=TEXT("https://api.test/items"); Result.bIdempotent=true;
        Result.IdempotencyKey=TEXT("stable-id"); Result.Headers.Set(TEXT("Idempotency-Key"),Result.IdempotencyKey);
        Result.Body=FVaRestRequestBody::FromBytes(TArray<uint8>{65,66,67},EVaRestBodyKind::Binary,TEXT("application/octet-stream"));
        Result.Headers.Set(TEXT("Content-Type"),Result.Body.ContentType);
        return Result;
    }
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestQueueStoreCodecBuffersTest,"VaRest.Http.QueueStore.CodecBuffers", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestQueueStoreCodecBuffersTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing;
    FScopedQueueCodecBuffers Probe;
    {
        auto Item=QueueStoreTestPrivate::Item(); Item.Body.Bytes.Init(173,32*1024*1024); Item.Body.ContentLength=Item.Body.Bytes.Num();
        FQueueCodecBufferOwner Original(EQueueCodecAccount::Body,Item.Body.Bytes);
        auto Encoded=EncodeQueueRecord(Item);
        if (!TestTrue(TEXT("hard maximum encoded"),!Encoded.IsEmpty())) return false;
        FQueueCodecBufferOwner Input(EQueueCodecAccount::Encoded,Encoded);
        auto Decoded=DecodeQueueRecord(Encoded,TEXT("https://api.test"),FVaRestOfflinePolicy());
        if (!TestTrue(TEXT("hard maximum decoded"),Decoded.HasValue())) return false;
        FQueueCodecBufferOwner Returned(EQueueCodecAccount::Body,Decoded.GetValue().Body.Bytes);
        TestTrue(TEXT("32 MiB bytes exact"),Decoded.GetValue().Body.Bytes==Item.Body.Bytes);
        TestTrue(TEXT("actual encoded allocation observed"),Probe.Buffers.Peak[0]>=Encoded.GetAllocatedSize());
        TestTrue(TEXT("input and output body owners observed"),Probe.Buffers.Peak[1]>=64LL*1024*1024);
        TestTrue(TEXT("additional backing buffers below metadata allowance"),Probe.Buffers.Peak[2]>0 && Probe.Buffers.Peak[2]<=2LL*1024*1024);
        TestEqual(TEXT("all parser backing tokens released"),Probe.Buffers.Current[2],int64(0));
    }
    for (int32 Account=0;Account<3;++Account) TestEqual(TEXT("all actual owners released"),Probe.Buffers.Current[Account],int64(0));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestQueueStoreSchemaTest,"VaRest.Http.QueueStore.Schema", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestQueueStoreSchemaTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    const auto Original=QueueStoreTestPrivate::Item();
    const auto Check=[this](FQueueItem Item,const TCHAR* Label)
    { TestTrue(Label,DecodeQueueRecord(EncodeQueueRecord(Item),TEXT("https://api.test"),FVaRestOfflinePolicy()).HasError()); };
    auto Item=Original; Item.Url=TEXT("https://other.test/items"); Check(Item,TEXT("actual forged origin refused"));
    Item=Original; Item.Sequence=0; Check(Item,TEXT("zero sequence"));
    Item=Original; Item.AuthMode=static_cast<EVaRestAuthMode>(3); Check(Item,TEXT("unknown auth mode"));
    Item=Original; Item.Tag=FString::ChrN(257,TEXT('t')); Check(Item,TEXT("oversized tag"));
    Item=Original; Item.Tag=TEXT("bad\n"); Check(Item,TEXT("control tag"));
    Item=Original; Item.Body.Kind=EVaRestBodyKind::Stream; Check(Item,TEXT("stream kind"));
    Item=Original; Item.Headers.Set(TEXT("Authorization"),TEXT("secret")); Check(Item,TEXT("credential field"));
    Item=Original; Item.Headers.Add(TEXT("content-type"),TEXT("application/octet-stream")); Check(Item,TEXT("duplicate header"));
    Item=Original; Item.Headers.Set(TEXT("Content-Type"),TEXT("text/plain")); Check(Item,TEXT("content type mismatch"));
    Item=Original; Item.Headers.Set(TEXT("Idempotency-Key"),TEXT("STABLE-ID")); Check(Item,TEXT("key byte case mismatch"));
    auto Bytes=EncodeQueueRecord(Original); if (!TestTrue(TEXT("valid fixture encoded"),!Bytes.IsEmpty())) return false; Bytes.Last()=0;
    TestTrue(TEXT("truncated grammar"),DecodeQueueRecord(Bytes,TEXT("https://api.test"),FVaRestOfflinePolicy()).HasError());
    bool bCapacity=false; Bytes=EncodeQueueRecord(Original);
    TestTrue(TEXT("capacity refuses before decoded ownership"),DecodeQueueRecord(Bytes,TEXT("https://api.test"),FVaRestOfflinePolicy(),2,&bCapacity).HasError());
    TestTrue(TEXT("capacity is distinct from corruption"),bCapacity);
    Item=Original; Item.Sequence=MAX_uint64;
    auto Exact=DecodeQueueRecord(EncodeQueueRecord(Item),TEXT("https://api.test"),FVaRestOfflinePolicy());
    if (!TestTrue(TEXT("max sequence schema"),Exact.HasValue())) return false;
    TestEqual(TEXT("max sequence exact"),Exact.GetValue().Sequence,MAX_uint64);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestQueueStoreRecoveryTest,"VaRest.Http.QueueStore.Recovery", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestQueueStoreRecoveryTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing;
    FScopedPersistenceTest Disk;
    auto Item=QueueStoreTestPrivate::Item(); const FString Directory=FPaths::Combine(Disk.Root,TEXT("records"));
    const auto Status=MakeShared<TOptional<bool>,ESPMode::ThreadSafe>();
    FQueueStore::Prepare(Directory,TEXT("https://api.test"),FVaRestOfflinePolicy(),Item,FPersistenceGuard(),[Status](bool bOk){*Status=bOk;});
    if (!TestTrue(TEXT("prepare drains"),Disk.Drain()) || !TestTrue(TEXT("prepare success"),Status->Get(false))) return false;
    const auto Loaded=MakeShared<FQueueLoad,ESPMode::ThreadSafe>();
    FQueueStore::Load(Directory,TEXT("https://api.test"),FVaRestOfflinePolicy(),[Loaded](FQueueLoad&& Value){*Loaded=MoveTemp(Value);});
    if (!TestTrue(TEXT("pending recovery drains"),Disk.Drain())) return false;
    TestTrue(TEXT("pending never replayed"),Loaded->bOk && Loaded->Items.IsEmpty());
    FQueueStore::Prepare(Directory,TEXT("https://api.test"),FVaRestOfflinePolicy(),Item,FPersistenceGuard(),[Status](bool bOk){*Status=bOk;});
    if (!TestTrue(TEXT("second prepare"),Disk.Drain()) || !TestTrue(TEXT("second prepared"),Status->Get(false))) return false;
    FQueueStore::Commit(Directory,Item.Sequence,Item.QueueId,[Status](FQueueCommitResult R){*Status=R.bCommitted;});
    if (!TestTrue(TEXT("commit drains"),Disk.Drain()) || !TestTrue(TEXT("commit success"),Status->Get(false))) return false;
    auto Small=FVaRestOfflinePolicy(); Small.MaxItemBytes=2;
    FQueueStore::Load(Directory,TEXT("https://api.test"),Small,[Loaded](FQueueLoad&& Value){*Loaded=MoveTemp(Value);});
    if (!TestTrue(TEXT("small policy drain"),Disk.Drain())) return false;
    TestTrue(TEXT("valid acknowledged overflow refuses whole recovery"),!Loaded->bOk && Loaded->bPolicyExceeded && Loaded->Items.IsEmpty());
    FQueueStore::Load(Directory,TEXT("https://api.test"),FVaRestOfflinePolicy(),[Loaded](FQueueLoad&& Value){*Loaded=MoveTemp(Value);});
    if (!TestTrue(TEXT("adequate policy drain"),Disk.Drain()) || !TestEqual(TEXT("acknowledged record preserved"),Loaded->Items.Num(),1)) return false;
    TestEqual(TEXT("next sequence"),Loaded->NextSequence,uint64(2));
    TestTrue(TEXT("body exact after recovery"),Loaded->Items[0].Body.Bytes==Item.Body.Bytes);
    const FString Json=FPaths::Combine(Directory,QueueFileName(Item.Sequence,Item.QueueId));
    const FString Tombstone=FPaths::ChangeExtension(Json,TEXT("done"));
    TestTrue(TEXT("tombstone admitted"),LaunchPersistence([Tombstone]{return WriteNewFile(Tombstone,TArray<uint8>{});},[](FIoResult&&){}));
    if (!TestTrue(TEXT("tombstone fixture drain"),Disk.Drain())) return false;
    FQueueStore::Load(Directory,TEXT("https://api.test"),FVaRestOfflinePolicy(),[Loaded](FQueueLoad&& Value){*Loaded=MoveTemp(Value);});
    if (!TestTrue(TEXT("tombstone recovery drain"),Disk.Drain())) return false;
    TestTrue(TEXT("done wins over json"),Loaded->bOk && Loaded->Items.IsEmpty());
    FQueueStore::Retire(Directory,Item.Sequence,Item.QueueId,[Status](bool bOk){*Status=bOk;});
    TestTrue(TEXT("missing retirement drains"),Disk.Drain()); TestTrue(TEXT("true missing is retired"),Status->Get(false));
    return true;
}
#if PLATFORM_LINUX
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestQueueStorePhysicalErrorsTest,"VaRest.Http.QueueStore.PhysicalErrors", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestQueueStorePhysicalErrorsTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http; using namespace VaRest::Http::Testing;
    FScopedPersistenceTest Disk; const FString Parent=FPaths::Combine(Disk.Root,TEXT("locked"));
    const FString Directory=FPaths::Combine(Parent,TEXT("queue")); auto Item=QueueStoreTestPrivate::Item();
    const FString Json=FPaths::Combine(Directory,QueueFileName(Item.Sequence,Item.QueueId));
    const FString Pending=FPaths::ChangeExtension(Json,TEXT("pending"));
    const auto Status=MakeShared<TOptional<bool>,ESPMode::ThreadSafe>();
    const auto Witness=MakeShared<bool,ESPMode::ThreadSafe>(false);
    ON_SCOPE_EXIT { LaunchPersistence([Parent]{ chmod(TCHAR_TO_UTF8(*Parent),0700); FIoResult R; R.bOk=true; return R; },[](FIoResult&&){}); Disk.Drain(); };
    FQueueStore::Prepare(Directory,TEXT("https://api.test"),FVaRestOfflinePolicy(),Item,FPersistenceGuard(),[Status](bool bOk){*Status=bOk;});
    if (!TestTrue(TEXT("permission fixture prepared"),Disk.Drain()) || !TestTrue(TEXT("prepared"),Status->Get(false))) return false;
    LaunchPersistence([Parent,Pending]{ FIoResult R; R.bOk=chmod(TCHAR_TO_UTF8(*Parent),0)==0 && InspectPersistencePath(Pending).Kind==EPathKind::Error; return R; },[Witness](FIoResult&& R){*Witness=R.bOk;});
    if (!TestTrue(TEXT("permission witness drains"),Disk.Drain()) || !TestTrue(TEXT("actual inaccessible parent witnessed"),*Witness)) return false;
    FQueueStore::Commit(Directory,Item.Sequence,Item.QueueId,[Status](FQueueCommitResult R){*Status=R.bCommitted;});
    if (!TestTrue(TEXT("inaccessible commit drains"),Disk.Drain())) return false;
    TestFalse(TEXT("unknown destination never committed"),Status->Get(true));
    FQueueStore::Retire(Directory,Item.Sequence,Item.QueueId,[Status](bool bOk){*Status=bOk;});
    if (!TestTrue(TEXT("inaccessible retire drains"),Disk.Drain())) return false;
    TestFalse(TEXT("unknown source never acknowledged missing"),Status->Get(true));
    LaunchPersistence([Parent,Pending]{ FIoResult R; R.bOk=chmod(TCHAR_TO_UTF8(*Parent),0700)==0 && InspectPersistencePath(Pending).Kind==EPathKind::Regular; return R; },[Witness](FIoResult&& R){*Witness=R.bOk;});
    if (!TestTrue(TEXT("restore permission drain"),Disk.Drain()) || !TestTrue(TEXT("failed cleanup preserved inaccessible source"),*Witness)) return false;
    FQueueStore::Commit(Directory,Item.Sequence,Item.QueueId,[Status](FQueueCommitResult R){*Status=R.bCommitted;});
    if (!TestTrue(TEXT("positive commit drains"),Disk.Drain()) || !TestTrue(TEXT("regular source commits"),Status->Get(false))) return false;
    FQueueStore::Retire(Directory,Item.Sequence,Item.QueueId,[Status](bool bOk){*Status=bOk;});
    if (!TestTrue(TEXT("positive retirement drains"),Disk.Drain()) || !TestTrue(TEXT("regular source retires"),Status->Get(false))) return false;
    LaunchPersistence([Pending]{ FIoResult R; R.bOk=mkfifo(TCHAR_TO_UTF8(*Pending),0600)==0; return R; },[Witness](FIoResult&& R){*Witness=R.bOk;});
    if (!TestTrue(TEXT("special fixture drain"),Disk.Drain()) || !TestTrue(TEXT("native FIFO created"),*Witness)) return false;
    FQueueStore::Commit(Directory,Item.Sequence,Item.QueueId,[Status](FQueueCommitResult R){*Status=R.bCommitted;});
    if (!TestTrue(TEXT("special commit drain"),Disk.Drain())) return false;
    TestFalse(TEXT("special source not moved"),Status->Get(true));
    LaunchPersistence([Pending]{ FIoResult R; R.bOk=InspectPersistencePath(Pending).Kind==EPathKind::Other; unlink(TCHAR_TO_UTF8(*Pending)); return R; },[Witness](FIoResult&& R){*Witness=R.bOk;});
    if (!TestTrue(TEXT("special preservation drain"),Disk.Drain())) return false;
    TestTrue(TEXT("special source not unlinked by failed cleanup"),*Witness);
    return true;
}
#endif

#if PLATFORM_LINUX
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestQueueStoreCleanupLoadTest,"VaRest.Http.QueueStore.CleanupLoad",EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestQueueStoreCleanupLoadTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;using namespace VaRest::Http::Testing;
    FScopedPersistenceTest Disk;const FString Directory=FPaths::Combine(Disk.Root,TEXT("cleanup-load"));
    auto Item=QueueStoreTestPrivate::Item();const auto Status=MakeShared<bool,ESPMode::ThreadSafe>(false);
    ON_SCOPE_EXIT {LaunchPersistence([Directory]{chmod(TCHAR_TO_UTF8(*PersistencePathForIo(Directory)),0700);return FIoResult();},[](FIoResult&&){});Disk.Drain();};
    FQueueStore::Prepare(Directory,TEXT("https://api.test"),{},Item,{},[Status](bool bOk){*Status=bOk;});
    if (!TestTrue(TEXT("prepare drain"),Disk.Drain()) || !TestTrue(TEXT("acknowledged fixture prepared"),*Status)) return false;
    FQueueStore::Commit(Directory,Item.Sequence,Item.QueueId,[Status](FQueueCommitResult R){*Status=R.bCommitted;});
    if (!TestTrue(TEXT("commit drain"),Disk.Drain()) || !TestTrue(TEXT("acknowledged fixture committed"),*Status)) return false;
    const FString Json=FPaths::Combine(Directory,QueueFileName(Item.Sequence,Item.QueueId));
    ++Item.Sequence;Item.QueueId=FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);
    const FString Pending=FPaths::ChangeExtension(FPaths::Combine(Directory,QueueFileName(Item.Sequence,Item.QueueId)),TEXT("pending"));
    FQueueStore::Prepare(Directory,TEXT("https://api.test"),{},Item,{},[Status](bool bOk){*Status=bOk;});
    if (!TestTrue(TEXT("pending prepare drain"),Disk.Drain()) || !TestTrue(TEXT("pending prepared"),*Status)) return false;
    LaunchPersistence([Directory,Pending]
    {
        FIoResult R;R.bOk=chmod(TCHAR_TO_UTF8(*PersistencePathForIo(Directory)),0500)==0
            && !DeleteFile(Pending).bOk && InspectPersistencePath(Pending).Kind==EPathKind::Regular;return R;
    },[Status](FIoResult&& R){*Status=R.bOk;});
    if (!TestTrue(TEXT("denial witness drain"),Disk.Drain()) || !TestTrue(TEXT("actual unlink denial witnessed"),*Status)) return false;
    const auto Loaded=MakeShared<FQueueLoad,ESPMode::ThreadSafe>();
    FQueueStore::Load(Directory,TEXT("https://api.test"),{},[Loaded](FQueueLoad&& R){*Loaded=MoveTemp(R);});
    if (!TestTrue(TEXT("failed cleanup load drains"),Disk.Drain())) return false;
    TestFalse(TEXT("load fails closed on pending cleanup error"),Loaded->bOk);TestTrue(TEXT("no partial acknowledged replay on failed recovery"),Loaded->Items.IsEmpty());
    LaunchPersistence([Directory,Pending,Json]
    {
        FIoResult R;R.bOk=InspectPersistencePath(Pending).Kind==EPathKind::Regular && InspectPersistencePath(Json).Kind==EPathKind::Regular;
        R.bOk=chmod(TCHAR_TO_UTF8(*PersistencePathForIo(Directory)),0700)==0 && R.bOk;return R;
    },[Status](FIoResult&& R){*Status=R.bOk;});
    if (!TestTrue(TEXT("restore drain"),Disk.Drain()) || !TestTrue(TEXT("failed cleanup preserved pending and acknowledged record"),*Status)) return false;
    FQueueStore::Load(Directory,TEXT("https://api.test"),{},[Loaded](FQueueLoad&& R){*Loaded=MoveTemp(R);});
    if (!TestTrue(TEXT("normal recovered load drains"),Disk.Drain())) return false;
    TestTrue(TEXT("recovery succeeds after actual cleanup"),Loaded->bOk);TestEqual(TEXT("acknowledged record preserved"),Loaded->Items.Num(),1);
    LaunchPersistence([Pending]{FIoResult R;R.bOk=InspectPersistencePath(Pending).Kind==EPathKind::Missing;return R;},[Status](FIoResult&& R){*Status=R.bOk;});
    TestTrue(TEXT("missing witness drains"),Disk.Drain());TestTrue(TEXT("recovery actually removed pending"),*Status);return true;
}
#endif

#endif
