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
#include "VaRestJson.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/LowLevelMemTracker.h"
#include "Containers/StringConv.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
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
        TestTrue(TEXT("actual encoded allocation observed"),Probe.Buffers.Peak[0]>=int64(Encoded.GetAllocatedSize()));
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
    FQueueStore::Prepare(Directory,TEXT("https://api.test"),FVaRestOfflinePolicy(),Item,FPersistenceGuard(),[Status](FQueuePrepareResult Result){*Status=Result.bPrepared;});
    if (!TestTrue(TEXT("prepare drains"),Disk.Drain()) || !TestTrue(TEXT("prepare success"),Status->Get(false))) return false;
    const auto Loaded=MakeShared<FQueueLoad,ESPMode::ThreadSafe>();
    FQueueStore::Load(Directory,TEXT("https://api.test"),FVaRestOfflinePolicy(),[Loaded](FQueueLoad&& Value){*Loaded=MoveTemp(Value);});
    if (!TestTrue(TEXT("pending recovery drains"),Disk.Drain())) return false;
    TestTrue(TEXT("pending never replayed"),Loaded->bOk && Loaded->Items.IsEmpty());
    FQueueStore::Prepare(Directory,TEXT("https://api.test"),FVaRestOfflinePolicy(),Item,FPersistenceGuard(),[Status](FQueuePrepareResult Result){*Status=Result.bPrepared;});
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
    FQueueStore::Prepare(Directory,TEXT("https://api.test"),FVaRestOfflinePolicy(),Item,FPersistenceGuard(),[Status](FQueuePrepareResult Result){*Status=Result.bPrepared;});
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
    FQueueStore::Prepare(Directory,TEXT("https://api.test"),{},Item,{},[Status](FQueuePrepareResult Result){*Status=Result.bPrepared;});
    if (!TestTrue(TEXT("prepare drain"),Disk.Drain()) || !TestTrue(TEXT("acknowledged fixture prepared"),*Status)) return false;
    FQueueStore::Commit(Directory,Item.Sequence,Item.QueueId,[Status](FQueueCommitResult R){*Status=R.bCommitted;});
    if (!TestTrue(TEXT("commit drain"),Disk.Drain()) || !TestTrue(TEXT("acknowledged fixture committed"),*Status)) return false;
    const FString Json=FPaths::Combine(Directory,QueueFileName(Item.Sequence,Item.QueueId));
    ++Item.Sequence;Item.QueueId=FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);
    const FString Pending=FPaths::ChangeExtension(FPaths::Combine(Directory,QueueFileName(Item.Sequence,Item.QueueId)),TEXT("pending"));
    FQueueStore::Prepare(Directory,TEXT("https://api.test"),{},Item,{},[Status](FQueuePrepareResult Result){*Status=Result.bPrepared;});
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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestQueuePrepareRefusalTest,"VaRest.Http.QueueStore.PrepareRefusalClean",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestQueuePrepareRefusalTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;using namespace VaRest::Http::Testing;
    FScopedPersistenceTest Disk;
    auto Item=QueueStoreTestPrivate::Item();Item.Url=TEXT("https://api.test/path?password=private");
    const auto Result=MakeShared<TOptional<FQueuePrepareResult>,ESPMode::ThreadSafe>();
    FQueueStore::Prepare(Disk.Root,TEXT("https://api.test"),{},Item,{},[Result](FQueuePrepareResult Value){*Result=Value;});
    if (!TestTrue(TEXT("validation result drains"),Disk.Drain()) || !TestTrue(TEXT("validation delivered"),Result->IsSet())) return false;
    TestTrue(TEXT("pre-IO validation refusal is clean"),!Result->GetValue().bPrepared && Result->GetValue().bCleanupOk);
    {
        FScopedPersistenceRuntime Runtime;Runtime.Stop();Result->Reset();
        const uint64 Before=PersistenceCounters().Launched.load();
        FQueueStore::Prepare(Disk.Root,TEXT("https://api.test"),{},Item,{},[Result](FQueuePrepareResult Value){*Result=Value;});
        if (!TestTrue(TEXT("launch refusal result delivered"),Result->IsSet())) return false;
        TestTrue(TEXT("launch refusal is clean"),!Result->GetValue().bPrepared && Result->GetValue().bCleanupOk);
        TestEqual(TEXT("refusal never launches worker"),PersistenceCounters().Launched.load(),Before);
    }
    return true;
}


namespace VaRest::Http::QueueCapBenchPrivate
{
    // This is the production scanner's work count, not a general JSON parser. The body is empty here.
    struct FQueueCapShape { int32 Units=0,MaxDepth=0; bool bOk=false; };
    FQueueCapShape Shape(TConstArrayView<uint8> Encoded)
    {
        FQueueCapShape Result; int32 Depth=0,Bodies=0;
        const auto White=[](uint8 Byte) { return Byte==' ' || Byte=='\t' || Byte=='\r' || Byte=='\n'; };
        for (int32 Index=0;Index<Encoded.Num();)
        {
            const uint8 Byte=Encoded[Index]; if (White(Byte)) { ++Index; continue; }
            ++Result.Units;
            if (Byte=='"')
            {
                const int32 Start=++Index; bool bEscaped=false;
                while (Index<Encoded.Num() && Encoded[Index]!='"')
                {
                    if (Encoded[Index]=='\\') { bEscaped=true;Index+=2; } else ++Index;
                }
                if (Index>=Encoded.Num()) return Result;
                const int32 Length=Index-Start; ++Index;
                if (Depth==1 && !bEscaped && Length==4 && FMemory::Memcmp(Encoded.GetData()+Start,"body",4)==0)
                {
                    while (Index<Encoded.Num() && White(Encoded[Index])) ++Index;
                    if (Index>=Encoded.Num() || Encoded[Index++]!=':') return Result;
                    while (Index<Encoded.Num() && White(Encoded[Index])) ++Index;
                    if (Index+1>=Encoded.Num() || Encoded[Index]!='"' || Encoded[Index+1]!='"') return Result;
                    Index+=2; ++Bodies; // SplitRecord skips this colon and body-value token too.
                }
                continue;
            }
            if (Byte=='{' || Byte=='[') Result.MaxDepth=FMath::Max(Result.MaxDepth,++Depth);
            else if (Byte=='}' || Byte==']') { if (--Depth<0) return Result; }
            ++Index;
        }
        Result.bOk=Depth==0 && Bodies==1; return Result;
    }
    int32 HeaderChars(const FQueueItem& Value)
    {
        int32 Total=0; for (const auto& Pair : Value.Headers) Total+=Pair.Key.Len()+Pair.Value.Len(); return Total;
    }
    FQueueItem Make(int32 EscapedChars,bool bAsciiTail)
    {
        FQueueItem Value=QueueStoreTestPrivate::Item();
        Value.CreatedUtc=FDateTime(2026,10,23,12,0,0);
        Value.Url=FString(TEXT("https://api.test/"))+FString::ChrN(8192-17,TEXT('a'));
        Value.IdempotencyKey=FString::ChrN(4096,TEXT('i'));
        Value.Tag=FString::ChrN(256,TEXT('t')); Value.Endpoint=FString::ChrN(256,TEXT('e'));
        const FString ContentType=FString(TEXT("application/"))+FString::ChrN(1024-12,TEXT('a'));
        Value.Body=FVaRestRequestBody::FromBytes(TArray<uint8>(),EVaRestBodyKind::Binary,ContentType);
        Value.Headers=FVaRestHeaders();
        Value.Headers.Add(TEXT("Idempotency-Key"),Value.IdempotencyKey);
        Value.Headers.Add(TEXT("Content-Type"),ContentType);
        for (int32 Header=0;Header<126;++Header)
        {
            const int32 Count=FMath::Min(EscapedChars,8192); EscapedChars-=Count;
            FString Text=FString::ChrN(Count,TEXT('\\'));
            if (Header==125 && bAsciiTail) Text.AppendChar(TEXT('a'));
            Value.Headers.Add(FString::Printf(TEXT("X%03d"),Header),Text);
        }
        check(EscapedChars==0); return Value;
    }
    double Median(TArray<double> Values)
    { Values.Sort();const int32 Middle=Values.Num()/2;return Values.Num()%2 ? Values[Middle] : (Values[Middle-1]+Values[Middle])*0.5; }
    double Maximum(const TArray<double>& Values)
    { double Value=0;for (double Sample : Values) Value=FMath::Max(Value,Sample);return Value; }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestM7QueueMetadataCapBenchTest,"Bench.VaRest.Http.QueueMetadataAdmission",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::PerfFilter)
bool FVaRestM7QueueMetadataCapBenchTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::QueueCapBenchPrivate;
    using namespace VaRest::Http::Testing;
    if (!TestTrue(TEXT("queue metadata benchmark runs on GT"),IsInGameThread())
        || !TestFalse(TEXT("timing excludes runtime LLM instrumentation"),LLM_IS_ENABLED())) return false;
    FString ReportPath;
    if (!FParse::Value(FCommandLine::Get(),TEXT("VaRestM7QueueCapReport="),ReportPath)
        || FPaths::IsRelative(ReportPath) || !IFileManager::Get().DirectoryExists(*FPaths::GetPath(ReportPath))
        || IFileManager::Get().FileExists(*ReportPath))
    { AddError(TEXT("Supply a fresh absolute -VaRestM7QueueCapReport= JSON file in an existing owned output directory"));return false; }
    const FVaRestOfflinePolicy Policy;
    FQueueItem CapItem; TArray<uint8> CapEncoded;
    // Preparation/search is untimed. Two parities let a one-byte ASCII tail reach an odd size too.
    for (bool bAsciiTail : {false,true})
    {
        const FQueueItem Empty=Make(0,bAsciiTail);
        int32 Low=0,High=32768-HeaderChars(Empty);
        while (Low<High)
        {
            const int32 Middle=Low+(High-Low+1)/2;
            if (EncodeQueueRecord(Make(Middle,bAsciiTail)).IsEmpty()) High=Middle-1; else Low=Middle;
        }
        FQueueItem Candidate=Make(Low,bAsciiTail);TArray<uint8> Bytes=EncodeQueueRecord(Candidate);
        if (Bytes.Num()>CapEncoded.Num()) { CapItem=MoveTemp(Candidate);CapEncoded=MoveTemp(Bytes); }
    }
    if (!TestEqual(TEXT("actual successful encoded metadata reaches 64KiB"),CapEncoded.Num(),65536)
        || !TestEqual(TEXT("maximum canonical header count"),CapItem.Headers.Num(),128)
        || !TestTrue(TEXT("body-free, no fake reserved charge or part-name padding"),CapItem.Body.Bytes.IsEmpty() && CapItem.Body.ContentLength==0 && CapItem.MetadataCharge==0 && CapItem.OriginalPartNames.IsEmpty())
        || !TestTrue(TEXT("valid header character cap"),HeaderChars(CapItem)<32768)
        || !TestTrue(TEXT("live DTO obeys existing charge gate"),QueueMetadataCharge(CapItem)<=256*1024)) return false;
    const FQueueCapShape CapShape=Shape(CapEncoded);
    if (!TestTrue(TEXT("real canonical scanner shape"),CapShape.bOk && CapShape.MaxDepth==3 && CapShape.Units<2048)) return false;
    auto Canonical=DecodeQueueRecord(CapEncoded,TEXT("https://api.test"),Policy,0);
    if (!TestTrue(TEXT("real body-free recovery accepts cap"),Canonical.HasValue())) return false;
    const int64 InputCharge=QueueMetadataCharge(CapItem),RecoveredCharge=QueueMetadataCharge(Canonical.GetValue());
    const auto Normalized=MeasureQueueAdmissionMetadata(CapItem,TEXT("https://api.test"),Policy);
    if (!TestTrue(TEXT("genuine normalization succeeds with exact live/recovered charge"),Normalized.IsSet() && Normalized.GetValue()==FMath::Max(InputCharge,RecoveredCharge))
        || !TestTrue(TEXT("canonical re-encode preserves actual record bytes"),EncodeQueueRecord(Canonical.GetValue())==CapEncoded)) return false;
    // Independent adjacent-size witness: change one legal, unescaped ASCII byte in the same header.
    FQueueItem TooLarge=CapItem;TooLarge.Headers.Set(TEXT("X125"),FString(TEXT("a"))+CapItem.Headers.Get(TEXT("X125")));
    TArray<uint8> TooLargeRecord=CapEncoded;const FTCHARToUTF8 Needle(TEXT("[\"X125\",\""));int32 Position=INDEX_NONE,Matches=0;
    for (int32 Index=0;Index+Needle.Length()<=CapEncoded.Num();++Index)
        if (FMemory::Memcmp(CapEncoded.GetData()+Index,Needle.Get(),Needle.Length())==0) { Position=Index+Needle.Length();++Matches; }
    if (!TestEqual(TEXT("one exact canonical header insertion location"),Matches,1)) return false;
    TooLargeRecord.Insert(uint8('a'),Position);
    if (!TestEqual(TEXT("adjacent raw metadata byte count"),TooLargeRecord.Num(),65537)
        || !TestTrue(TEXT("adjacent DTO remains within other size gates"),HeaderChars(TooLarge)<=32768 && TooLarge.Headers.Num()==128 && QueueMetadataCharge(TooLarge)<=256*1024)
        || !TestTrue(TEXT("encoder and genuine normalization refuse one byte beyond cap"),EncodeQueueRecord(TooLarge).IsEmpty() && !MeasureQueueAdmissionMetadata(TooLarge,TEXT("https://api.test"),Policy).IsSet())
        || !TestTrue(TEXT("strict decoder refuses actual 65537-byte record"),DecodeQueueRecord(TooLargeRecord,TEXT("https://api.test"),Policy,0).HasError())
        || !TestTrue(TEXT("same-origin oracle remains active"),!MeasureQueueAdmissionMetadata(CapItem,TEXT("https://other.test"),Policy).IsSet())) return false;
    int64 CodecPeak[3]={0,0,0};
    {
        FScopedQueueCodecBuffers Probe;
        const auto Observed=MeasureQueueAdmissionMetadata(CapItem,TEXT("https://api.test"),Policy);
        if (!TestTrue(TEXT("untimed resource observation succeeds"),Observed.IsSet())) return false;
        for (int32 Account=0;Account<3;++Account)
        {
            CodecPeak[Account]=Probe.Buffers.Peak[Account];
            if (!TestEqual(TEXT("annotated codec owners drain"),Probe.Buffers.Current[Account],int64(0))) return false;
        }
    }
    if (!TestTrue(TEXT("actual encoded owner observed"),CodecPeak[0]>=CapEncoded.Num())
        || !TestEqual(TEXT("no body backing allocated"),CodecPeak[1],int64(0))
        || !TestTrue(TEXT("annotated additional backing within allowance"),CodecPeak[2]>0 && CodecPeak[2]<=2LL*1024*1024)) return false;
    // All 64 caller DTOs and every result/timing slot are prepared outside the timed calls.
    TArray<FQueueItem> Inputs;Inputs.Reserve(64);
    for (int32 Index=0;Index<64;++Index)
    { FQueueItem Value=CapItem;Value.QueueId=FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);Inputs.Add(MoveTemp(Value)); }
    FVaRestJson BurstRows=FVaRestJson::Array();
    for (int32 Burst : {1,8,64})
    {
        TArray<double> Totals,PerCall;Totals.Reserve(31);PerCall.Reserve(31*Burst);
        TArray<double> CallMs;CallMs.SetNumUninitialized(Burst);TArray<TOptional<int64>> Charges;Charges.SetNum(Burst);
        int64 MinCharge=MAX_int64,MaxCharge=0;
        for (int32 Sample=-1;Sample<31;++Sample)
        {
            if (!TestTrue(TEXT("each sequential burst is on GT"),IsInGameThread())) return false;
            for (int32 Index=0;Index<Burst;++Index)
            {
                const double Start=FPlatformTime::Seconds();
                const auto Charge=MeasureQueueAdmissionMetadata(Inputs[Index],TEXT("https://api.test"),Policy);
                const double Elapsed=FPlatformTime::Seconds()-Start;
                Charges[Index]=Charge;CallMs[Index]=Elapsed*1000.0;
            }
            double Total=0;
            for (int32 Index=0;Index<Burst;++Index)
            {
                if (!TestTrue(TEXT("every measured call succeeds, finite time and bounded charge"),Charges[Index].IsSet() && Charges[Index].GetValue()<=256*1024 && FMath::IsFinite(CallMs[Index]) && CallMs[Index]>=0)) return false;
                Total+=CallMs[Index];MinCharge=FMath::Min(MinCharge,Charges[Index].GetValue());MaxCharge=FMath::Max(MaxCharge,Charges[Index].GetValue());
                if (Sample>=0) PerCall.Add(CallMs[Index]);
            }
            if (Sample>=0) Totals.Add(Total); // The complete first burst is an excluded warm-up.
        }
        FVaRestJson Row=FVaRestJson::Object();Row.Set(TEXT("burst"),Burst);Row.Set(TEXT("warmup_bursts_excluded"),1);Row.Set(TEXT("measured_bursts"),31);
        Row.Set(TEXT("measured_calls"),31*Burst);Row.Set(TEXT("normalization_only_burst_ms_median"),Median(Totals));Row.Set(TEXT("normalization_only_burst_ms_max"),Maximum(Totals));
        Row.Set(TEXT("per_call_ms_median"),Median(PerCall));Row.Set(TEXT("per_call_ms_max"),Maximum(PerCall));Row.Set(TEXT("returned_charge_bytes_min"),MinCharge);Row.Set(TEXT("returned_charge_bytes_max"),MaxCharge);
        FVaRestJson Samples=FVaRestJson::Array();for (double Value : Totals) Samples.Add(Value);Row.Set(TEXT("normalization_only_burst_samples_ms"),Samples);BurstRows.Add(Row);
    }
    if (!TestTrue(TEXT("normalization leaves input unchanged and completes on GT"),IsInGameThread() && EncodeQueueRecord(CapItem)==CapEncoded)) return false;
    FVaRestJson QueueCapReport=FVaRestJson::Object();QueueCapReport.Set(TEXT("schema"),TEXT("VaRest.M7.QueueMetadataCap.1"));QueueCapReport.Set(TEXT("genuine_body_free_normalization"),true);
    QueueCapReport.Set(TEXT("encoded_bytes"),CapEncoded.Num());QueueCapReport.Set(TEXT("header_count"),CapItem.Headers.Num());QueueCapReport.Set(TEXT("header_chars"),HeaderChars(CapItem));
    QueueCapReport.Set(TEXT("queue_scanner_lexical_units"),CapShape.Units);QueueCapReport.Set(TEXT("canonical_depth"),CapShape.MaxDepth);QueueCapReport.Set(TEXT("input_DTO_charge_bytes"),InputCharge);QueueCapReport.Set(TEXT("canonical_DTO_charge_bytes"),RecoveredCharge);
    QueueCapReport.Set(TEXT("annotated_encoded_peak_bytes"),CodecPeak[0]);QueueCapReport.Set(TEXT("annotated_body_peak_bytes"),CodecPeak[1]);QueueCapReport.Set(TEXT("annotated_additional_backing_peak_bytes"),CodecPeak[2]);
    QueueCapReport.Set(TEXT("temporary_logical_allowance_bytes"),2*1024*1024);QueueCapReport.Set(TEXT("full_temporary_arena_or_RSS_peak_measured"),false);QueueCapReport.Set(TEXT("timed_codec_probe_active"),false);QueueCapReport.Set(TEXT("runtime_LLM_enabled"),false);
    QueueCapReport.Set(TEXT("game_thread"),true);QueueCapReport.Set(TEXT("adjacent_65537_refused"),true);QueueCapReport.Set(TEXT("wrong_origin_refused"),true);QueueCapReport.Set(TEXT("bursts"),BurstRows);
    QueueCapReport.Set(TEXT("limits"),TEXT("Timing sums only synchronous MeasureQueueAdmissionMetadata calls; excludes fixture prep, assertions, result processing and dispatch gaps. One warm-up burst excluded. Codec resource pass is untimed and observes annotated backing only, not JSON arena/DTO allocations or RSS; no full2MiB saturation/peak claim. Valid19-field schema with128 two-string header pairs reaches depth3 and fewer than2048 scanner units, not depth8/2048. No frame-time, throughput floor or end-to-end admission/persistence guarantee."));
    const FString QueueCapText=QueueCapReport.ToString();
    if (!TestTrue(TEXT("write fresh owned cap report without overwriting"),FFileHelper::SaveStringToFile(QueueCapText,*ReportPath,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,&IFileManager::Get(),FILEWRITE_NoReplaceExisting))) return false;
    AddInfo(FString::Printf(TEXT("VAREST_M7_QUEUE_METADATA_CAP_REPORT %s"),*ReportPath));return true;
}

#endif
