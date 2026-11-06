// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestQueueStore.h"
#include "VaRestHttpTesting.h"
#include "VaRestIdentity.h"
#include "VaRestJson.h"
#include "VaRestSha256.h"
#include "VaRestUrl.h"
#include "VaRestPersistence.h"
#include "Misc/Base64.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformFileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace VaRest::Http::Testing
{
    namespace QueueCodecPrivate { thread_local FQueueCodecBuffers* Active=nullptr; }
    FScopedQueueCodecBuffers::FScopedQueueCodecBuffers() : Previous(QueueCodecPrivate::Active)
    { QueueCodecPrivate::Active=&Buffers; }
    FScopedQueueCodecBuffers::~FScopedQueueCodecBuffers()
    {
        check(Buffers.Current[0]==0 && Buffers.Current[1]==0 && Buffers.Current[2]==0);
        QueueCodecPrivate::Active=Previous;
    }
    void FQueueCodecBufferOwner::Acquire(EQueueCodecAccount Account,int64 AllocatedBytes)
    {
        Probe=QueueCodecPrivate::Active; AccountIndex=static_cast<int32>(Account); Bytes=AllocatedBytes;
        if (Probe)
        {
            Probe->Current[AccountIndex]+=Bytes;
            Probe->Peak[AccountIndex]=FMath::Max(Probe->Peak[AccountIndex],Probe->Current[AccountIndex]);
        }
    }
    FQueueCodecBufferOwner::FQueueCodecBufferOwner(EQueueCodecAccount Account,const TArray<uint8>& Owner)
    { Acquire(Account,Owner.GetAllocatedSize()); }
    FQueueCodecBufferOwner::FQueueCodecBufferOwner(EQueueCodecAccount Account,const FString& Owner)
    { Acquire(Account,Owner.GetAllocatedSize()); }
    FQueueCodecBufferOwner::~FQueueCodecBufferOwner()
    { if (Probe) { Probe->Current[AccountIndex]-=Bytes; check(Probe->Current[AccountIndex]>=0); } }
}
#endif

namespace VaRest::Http::QueueStorePrivate
{
    struct FRecordParts { int32 BodyStart=0,BodyLength=0; TArray<uint8> Metadata; };
    bool White(uint8 Byte) { return Byte==' ' || Byte=='\t' || Byte=='\r' || Byte=='\n'; }
    bool SplitRecord(TConstArrayView<uint8> Bytes,FRecordParts& Out)
    {
        int32 Depth=0,Units=0,BodyStart=INDEX_NONE,BodyEnd=INDEX_NONE;
        for (int32 Index=0;Index<Bytes.Num();)
        {
            const uint8 Byte=Bytes[Index];
            if (White(Byte)) { ++Index; continue; }
            if (++Units>2048) return false; // strings + nonwhitespace punctuation/primitive bytes
            if (Byte=='"')
            {
                const int32 Start=++Index;
                bool bEscaped=false;
                while (Index<Bytes.Num() && Bytes[Index]!='"')
                {
                    if (Bytes[Index]=='\\') { bEscaped=true; Index+=2; }
                    else ++Index;
                }
                if (Index>=Bytes.Num()) return false;
                const int32 Length=Index-Start; ++Index;
                int32 Colon=Index; while (Colon<Bytes.Num() && White(Bytes[Colon])) ++Colon;
                const bool bBodyKey=Depth==1 && !bEscaped && Length==4
                    && Bytes[Start]=='b' && Bytes[Start+1]=='o' && Bytes[Start+2]=='d' && Bytes[Start+3]=='y'
                    && Colon<Bytes.Num() && Bytes[Colon]==':';
                if (!bBodyKey) continue;
                if (BodyStart!=INDEX_NONE) return false;
                int32 Value=Colon+1; while (Value<Bytes.Num() && White(Bytes[Value])) ++Value;
                if (Value>=Bytes.Num() || Bytes[Value]!='"') return false;
                BodyStart=++Value;
                while (Value<Bytes.Num() && Bytes[Value]!='"')
                {
                    const uint8 Code=Bytes[Value];
                    if (!((Code>='A' && Code<='Z') || (Code>='a' && Code<='z')
                        || (Code>='0' && Code<='9') || Code=='+' || Code=='/' || Code=='=')) return false;
                    ++Value;
                }
                if (Value>=Bytes.Num()) return false;
                BodyEnd=Value; Index=Value+1; continue;
            }
            if (Byte=='{' || Byte=='[') { if (++Depth>8) return false; }
            else if (Byte=='}' || Byte==']') { if (--Depth<0) return false; }
            ++Index;
        }
        if (Depth!=0 || BodyStart==INDEX_NONE || Bytes.Num()-(BodyEnd-BodyStart)>65536) return false;
        Out.BodyStart=BodyStart; Out.BodyLength=BodyEnd-BodyStart;
        Out.Metadata.Reserve(Bytes.Num()-Out.BodyLength+1);
        Out.Metadata.Append(Bytes.GetData(),BodyStart);
        Out.Metadata.Append(Bytes.GetData()+BodyEnd,Bytes.Num()-BodyEnd);
        return true; // strict JSON parser below validates the complete remaining grammar
    }
}

namespace VaRest::Http::QueueStorePrivate
{
    bool Decimal(FStringView Text,uint64& Out)
    {
        if (Text.IsEmpty()) return false;
        uint64 Value=0;
        for (TCHAR Character : Text)
        {
            if (Character<TEXT('0') || Character>TEXT('9')) return false;
            const uint64 Digit=Character-TEXT('0');
            if (Value>(MAX_uint64-Digit)/10) return false;
            Value=Value*10+Digit;
        }
        Out=Value; return true;
    }
    bool Hex(FStringView Text,int32 Length)
    {
        if (Text.Len()!=Length) return false;
        for (TCHAR Character : Text)
            if (!((Character>=TEXT('0') && Character<=TEXT('9')) || (Character>=TEXT('a') && Character<=TEXT('f')))) return false;
        return true;
    }
    bool Plain(FStringView Text,int32 Limit)
    {
        if (Text.Len()>Limit) return false;
        for (TCHAR Character : Text) if (Character<32 || Character==127) return false;
        return true;
    }
    bool SafeVerb(FStringView Verb)
    {
        for (const TCHAR* Allowed : {TEXT("GET"),TEXT("HEAD"),TEXT("PUT"),TEXT("DELETE"),TEXT("OPTIONS"),TEXT("TRACE")})
            if (Verb==Allowed) return true;
        return false;
    }
    int64 RecordLimit(const FVaRestOfflinePolicy& Policy) { return ((FMath::Clamp<int64>(Policy.MaxItemBytes,0,32LL*1024*1024)+2)/3)*4+65536; }
    bool FileIdentity(FStringView Name,uint64& Sequence,FString& Id,FStringView Extension)
    {
        const FString Text(Name);
        if (Text.Len()!=53+Extension.Len() || Text[20]!=TEXT('-') || !Text.EndsWith(FString(Extension),ESearchCase::CaseSensitive)) return false;
        Id=Text.Mid(21,32);
        FGuid Parsed;
        return Decimal(Text.Left(20),Sequence) && Sequence!=0 && Hex(Id,32)
            && FGuid::ParseExact(Id,EGuidFormats::Digits,Parsed) && Parsed.IsValid();
    }
}
TValueOrError<VaRest::Http::FQueueItem,FString> VaRest::Http::DecodeQueueRecord(
    TConstArrayView<uint8> Bytes,FStringView ExpectedOrigin,const FVaRestOfflinePolicy& Policy,
    int64 AvailableBody,bool* bCapacityRefused)
{
    if (bCapacityRefused) *bCapacityRefused=false;
    const auto Refuse=[]() -> TValueOrError<FQueueItem,FString> { return MakeError(FString(TEXT("QueueRecord"))); };
    if (Bytes.Num()>QueueStorePrivate::RecordLimit(Policy)) return Refuse();
    QueueStorePrivate::FRecordParts Parts;
    if (!QueueStorePrivate::SplitRecord(Bytes,Parts)) return Refuse();
    #if WITH_DEV_AUTOMATION_TESTS
    // ParseOwned takes this exact backing allocation; keep it charged through Parsed/Root destruction.
    Testing::FQueueCodecBufferOwner MetadataOwner(Testing::EQueueCodecAccount::Additional,Parts.Metadata);
#endif
    FVaRestJsonParseOptions Options; Options.MaxInputBytes=65536; Options.MaxDepth=8;
    auto Parsed=FVaRestJson::ParseOwned(MoveTemp(Parts.Metadata),Options);
    if (Parsed.HasError()) return Refuse();
    const auto Root=Parsed.StealValue();
    if (!Root.IsObject() || !Root[TEXT("version")].IsNumber()) return Refuse();
    const double Version=Root[TEXT("version")].AsDouble();
    const bool bV2=Version==2.0;
    if ((!bV2 && Version!=1.0) || Root.Num()!=(bV2 ? 19 : 18)) return Refuse();
    TArray<FString> Allowed{TEXT("version"),TEXT("queueId"),TEXT("sequence"),TEXT("runId"),TEXT("originalRequestId"),
        TEXT("createdUtc"),TEXT("scope"),TEXT("verb"),TEXT("url"),TEXT("authMode"),TEXT("idempotent"),TEXT("idempotencyKey"),
        TEXT("tag"),TEXT("bodyKind"),TEXT("contentType"),TEXT("body"),TEXT("bodySha256"),TEXT("headers")};
    if (bV2) Allowed.Add(TEXT("endpoint"));
    TSet<FString> Seen;
    for (const auto& Member : Root.Members())
    {
        const FString Key=Member.GetKey();
        if (!Allowed.ContainsByPredicate([&Key](const FString& Candidate){ return Candidate.Equals(Key,ESearchCase::CaseSensitive); })
            || Seen.Contains(Key)) return Refuse();
        Seen.Add(Key);
    }
    for (const TCHAR* Field : {TEXT("queueId"),TEXT("sequence"),TEXT("runId"),TEXT("originalRequestId"),
        TEXT("createdUtc"),TEXT("scope"),TEXT("verb"),TEXT("url"),TEXT("idempotencyKey"),TEXT("tag"),
        TEXT("contentType"),TEXT("body"),TEXT("bodySha256")}) if (!Root[Field].IsString()) return Refuse();
    const auto Mode=Root[TEXT("authMode")].TryGet<uint8>();
    const auto Kind=Root[TEXT("bodyKind")].TryGet<uint8>();
    if (!Mode.IsSet() || *Mode>static_cast<uint8>(EVaRestAuthMode::None)
        || !Kind.IsSet() || *Kind>static_cast<uint8>(EVaRestBodyKind::Binary)
        || !Root[TEXT("idempotent")].IsBool()) return Refuse();
    FQueueItem Item;
    if (bV2)
    {
        if (!Root[TEXT("endpoint")].IsString()) return Refuse();
        Item.Endpoint=Root[TEXT("endpoint")].AsString();
        if (!QueueStorePrivate::Plain(Item.Endpoint,256)) return Refuse();
    }
    Item.QueueId=Root[TEXT("queueId")].AsString();
    FGuid Id;
    if (!QueueStorePrivate::Hex(Item.QueueId,32) || !FGuid::ParseExact(Item.QueueId,EGuidFormats::Digits,Id) || !Id.IsValid()
        || !QueueStorePrivate::Decimal(Root[TEXT("sequence")].AsString(),Item.Sequence) || Item.Sequence==0
        || !QueueStorePrivate::Decimal(Root[TEXT("originalRequestId")].AsString(),Item.OriginalRequestId)) return Refuse();
    const FString Run=Root[TEXT("runId")].AsString();
    if (!QueueStorePrivate::Hex(Run,32) || !FGuid::ParseExact(Run,EGuidFormats::Digits,Item.OriginalRun)
        || !Item.OriginalRun.IsValid() || !FDateTime::ParseIso8601(*Root[TEXT("createdUtc")].AsString(),Item.CreatedUtc)) return Refuse();
    Item.Scope=Root[TEXT("scope")].AsString();
    Item.Verb=Root[TEXT("verb")].AsString(); Item.Url=Root[TEXT("url")].AsString();
    Item.Tag=Root[TEXT("tag")].AsString(); Item.IdempotencyKey=Root[TEXT("idempotencyKey")].AsString();
    Item.AuthMode=static_cast<EVaRestAuthMode>(*Mode); Item.bIdempotent=Root[TEXT("idempotent")].AsBool();
    if (!QueueStorePrivate::Hex(Item.Scope,64) || !QueueStorePrivate::Plain(Item.Tag,256)
        || !QueueStorePrivate::Plain(Item.Url,8192) || !SameOrigin(Item.Url,ExpectedOrigin)
        || !IsValidHeaderName(Item.Verb) || (!QueueStorePrivate::SafeVerb(Item.Verb) && !Item.bIdempotent)
        || !QueueStorePrivate::Plain(Item.IdempotencyKey,4096) || !IsValidHeaderValue(Item.IdempotencyKey)) return Refuse();
    const auto Headers=Root[TEXT("headers")];
    if (!Headers.IsArray() || Headers.Num()>128) return Refuse();
    int64 HeaderChars=0;
    for (const auto& Pair : Headers.Elements())
    {
        if (!Pair.IsArray() || Pair.Num()!=2 || !Pair[0].IsString() || !Pair[1].IsString()) return Refuse();
        const FString Name=Pair[0].AsString(),Value=Pair[1].AsString();
        if (Name.Len()>256 || Value.Len()>8192 || HeaderChars+Name.Len()+Value.Len()>32768
            || !IsValidHeaderName(Name) || !IsValidHeaderValue(Value) || Item.Headers.Contains(Name)) return Refuse();
        HeaderChars+=Name.Len()+Value.Len(); Item.Headers.Add(Name,Value);
    }
    FPersistenceGuard Structural;
    if (Structural.SafeHeaders(Item.Headers).Num()!=Item.Headers.Num() || !Structural.AllowsUrl(Item.Url)) return Refuse();
    if (Item.bIdempotent && !QueueStorePrivate::SafeVerb(Item.Verb)
        && (Item.IdempotencyKey.IsEmpty() || !Item.Headers.Get(TEXT("Idempotency-Key")).Equals(Item.IdempotencyKey,ESearchCase::CaseSensitive))) return Refuse();
    const FString ContentType=Root[TEXT("contentType")].AsString();
    if (!QueueStorePrivate::Plain(ContentType,1024) || !IsValidHeaderValue(ContentType)
        || (Item.Headers.Contains(TEXT("Content-Type")) && !Item.Headers.Get(TEXT("Content-Type")).Equals(ContentType,ESearchCase::CaseSensitive))) return Refuse();
    const UTF8CHAR* Encoded=reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()+Parts.BodyStart);
    const uint32 EncodedLength=Parts.BodyLength;
    if (EncodedLength%4!=0 || EncodedLength>FBase64::GetEncodedDataSize(Policy.MaxItemBytes)) return Refuse();
    const uint32 DecodedLength=FBase64::GetDecodedDataSize(Encoded,EncodedLength);
    if (DecodedLength>AvailableBody)
    { if (bCapacityRefused) *bCapacityRefused=true; return Refuse(); }
    if (DecodedLength>Policy.MaxItemBytes) return Refuse();
    // Strict padding/canonical low bits; the engine decoder alone accepts some noncanonical encodings.
    const auto Digit=[](UTF8CHAR Code)->int32
    {
        if (Code>='A' && Code<='Z') return Code-'A';
        if (Code>='a' && Code<='z') return Code-'a'+26;
        if (Code>='0' && Code<='9') return Code-'0'+52;
        return Code=='+' ? 62 : (Code=='/' ? 63 : -1);
    };
    uint32 Padding=0;
    if (EncodedLength>0 && Encoded[EncodedLength-1]=='=') ++Padding;
    if (EncodedLength>1 && Encoded[EncodedLength-2]=='=') ++Padding;
    for (uint32 Index=0;Index<EncodedLength-Padding;++Index) if (Digit(Encoded[Index])<0) return Refuse();
    if ((Padding==2 && (Digit(Encoded[EncodedLength-3])&15)!=0)
        || (Padding==1 && (Digit(Encoded[EncodedLength-2])&3)!=0)) return Refuse();
    TArray<uint8> Body; Body.SetNumUninitialized(DecodedLength);
#if WITH_DEV_AUTOMATION_TESTS
    Testing::FQueueCodecBufferOwner BodyOwner(Testing::EQueueCodecAccount::Body,Body);
#endif
    if ((EncodedLength>0 && !FBase64::Decode(Encoded,EncodedLength,Body.GetData()))
        || FSha256::Hash(Body).ToHex()!=Root[TEXT("bodySha256")].AsString()) return Refuse();
    if (*Kind==static_cast<uint8>(EVaRestBodyKind::None) && !Body.IsEmpty()) return Refuse();
    Item.Body=FVaRestRequestBody::FromBytes(MoveTemp(Body),static_cast<EVaRestBodyKind>(*Kind),ContentType);
    return MakeValue(MoveTemp(Item));
}
FGuid VaRest::Http::QueueRunId()
{
    static const FGuid Id=FGuid::NewGuid(); return Id;
}

TArray<uint8> VaRest::Http::EncodeQueueRecord(const FQueueItem& Item)
{
    if (Item.Body.Bytes.Num()>32*1024*1024 || QueueMetadataCharge(Item)>256*1024 || Item.Headers.Num()>128
        || !QueueStorePrivate::Plain(Item.Endpoint,256)) return {};
    FVaRestJson Root = FVaRestJson::Object();
    Root.Set(TEXT("version"), 2);
    Root.Set(TEXT("queueId"), Item.QueueId);
    Root.Set(TEXT("sequence"), LexToString(Item.Sequence));
    Root.Set(TEXT("runId"), Item.OriginalRun.ToString(EGuidFormats::DigitsLower));
    Root.Set(TEXT("originalRequestId"), LexToString(Item.OriginalRequestId));
    Root.Set(TEXT("createdUtc"), Item.CreatedUtc.ToIso8601());
    Root.Set(TEXT("scope"), Item.Scope);
    Root.Set(TEXT("verb"), Item.Verb);
    Root.Set(TEXT("url"), Item.Url);
    Root.Set(TEXT("authMode"), static_cast<int32>(Item.AuthMode));
    Root.Set(TEXT("idempotent"), Item.bIdempotent);
    Root.Set(TEXT("idempotencyKey"), Item.IdempotencyKey);
    Root.Set(TEXT("tag"), Item.Tag);
    Root.Set(TEXT("endpoint"), Item.Endpoint);
    Root.Set(TEXT("bodyKind"), static_cast<int32>(Item.Body.Kind));
    Root.Set(TEXT("contentType"), Item.Body.ContentType);
    Root.Set(TEXT("body"), TEXT(""));
    Root.Set(TEXT("bodySha256"), FSha256::Hash(Item.Body.Bytes).ToHex());
    FVaRestJson Headers = FVaRestJson::Array();
    for (const auto& Pair : Item.Headers) Headers.Add(FVaRestJson::Array(Pair.Key, Pair.Value));
    Root.Set(TEXT("headers"), Headers);
    TArray<uint8> Metadata;
    if (Root.TryToUtf8(Metadata).HasError() || Metadata.Num()>65536) return {};
    #if WITH_DEV_AUTOMATION_TESTS
    Testing::FQueueCodecBufferOwner MetadataOwner(Testing::EQueueCodecAccount::Additional,Metadata);
#endif
    QueueStorePrivate::FRecordParts Parts;
    if (!QueueStorePrivate::SplitRecord(Metadata,Parts) || Parts.BodyLength!=0) return {};
    #if WITH_DEV_AUTOMATION_TESTS
    Testing::FQueueCodecBufferOwner SplitOwner(Testing::EQueueCodecAccount::Additional,Parts.Metadata);
#endif
    const uint32 EncodedLength=FBase64::GetEncodedDataSize(Item.Body.Bytes.Num());
    TArray<uint8> Bytes; Bytes.SetNumUninitialized(Metadata.Num()+EncodedLength);
#if WITH_DEV_AUTOMATION_TESTS
    Testing::FQueueCodecBufferOwner EncodedOwner(Testing::EQueueCodecAccount::Encoded,Bytes);
#endif
    FMemory::Memcpy(Bytes.GetData(),Metadata.GetData(),Parts.BodyStart);
    FBase64::Encode(Item.Body.Bytes.GetData(),Item.Body.Bytes.Num(),reinterpret_cast<UTF8CHAR*>(Bytes.GetData()+Parts.BodyStart));
    FMemory::Memcpy(Bytes.GetData()+Parts.BodyStart+EncodedLength,Metadata.GetData()+Parts.BodyStart,Metadata.Num()-Parts.BodyStart);
    return Bytes;
}
FString VaRest::Http::QueueFileName(uint64 Sequence, FStringView QueueId)
{
    return FString::Printf(TEXT("%020llu-%s.json"), Sequence, *FString(QueueId));
}

namespace VaRest::Http::QueueStorePrivate
{
    bool AsyncBool(FIoWork&& Work,TUniqueFunction<void(bool)>&& Done,int64 Reservation=0,bool bControlDuringStop=false)
    {
        auto Callback=MakeShared<TUniqueFunction<void(bool)>,ESPMode::ThreadSafe>(MoveTemp(Done));
        if (!LaunchPersistence(MoveTemp(Work),[Callback](FIoResult&& Result){ (*Callback)(Result.bOk); },Reservation,bControlDuringStop))
		{ (*Callback)(false); return false; }
		return true;
    }
    FString PendingPath(const FString& Directory,uint64 Sequence,const FString& Id)
    { return FPaths::ChangeExtension(FPaths::Combine(Directory,QueueFileName(Sequence,Id)),TEXT("pending")); }
}
void VaRest::Http::FQueueStore::Prepare(FString Directory,FString Origin,FVaRestOfflinePolicy Policy,
    FQueueItem Item,FPersistenceGuard Guard,TUniqueFunction<void(FQueuePrepareResult)>&& Done)
{
    const int64 Reservation=FMath::Max<int64>(
        PersistenceGuardReservation(Item.Body.Kind,Item.Body.ContentType,Item.Body.Bytes.Num()),
        2LL*Item.Body.Bytes.Num()+QueueStorePrivate::RecordLimit(Policy)+2*1024*1024);
#if WITH_DEV_AUTOMATION_TESTS
    const auto Hooks=Testing::PersistenceHooksFor(Directory);
#endif
    const auto Callback=MakeShared<TUniqueFunction<void(FQueuePrepareResult)>,ESPMode::ThreadSafe>(MoveTemp(Done));
    if (!LaunchPersistence([Directory=MoveTemp(Directory),Origin=MoveTemp(Origin),Policy,
        Item=MoveTemp(Item),Guard=MoveTemp(Guard)
#if WITH_DEV_AUTOMATION_TESTS
        ,Hooks
#endif
    ]() mutable
    {
        FIoResult Refused;
        for (const FString& Name : Item.OriginalPartNames)
            if (Guard.Rules.ShouldRedact(Name,EVaRestRedactKeyKind::Part)) return Refused;
        if (!Guard.AllowsUrl(Item.Url) || !Guard.AllowsBody(Item.Body.Kind,Item.Body.ContentType,Item.Body.Bytes)
            || Guard.SafeHeaders(Item.Headers).Num()!=Item.Headers.Num()) return Refused;
        // Scan every serialized metadata value and raw bytes before base64 can hide a known value.
        FQueueItem Metadata;
        Metadata.QueueId=Item.QueueId; Metadata.Sequence=Item.Sequence; Metadata.OriginalRun=Item.OriginalRun;
        Metadata.OriginalRequestId=Item.OriginalRequestId; Metadata.CreatedUtc=Item.CreatedUtc;
        Metadata.Scope=Item.Scope; Metadata.Verb=Item.Verb; Metadata.Url=Item.Url; Metadata.Headers=Item.Headers;
        Metadata.IdempotencyKey=Item.IdempotencyKey; Metadata.Tag=Item.Tag; Metadata.AuthMode=Item.AuthMode;
        Metadata.Endpoint=Item.Endpoint;
        Metadata.bIdempotent=Item.bIdempotent;
        Metadata.Body=FVaRestRequestBody::FromBytes(TArray<uint8>(),Item.Body.Kind,Item.Body.ContentType);
        const auto MetaBytes=EncodeQueueRecord(Metadata);
        if (MetaBytes.IsEmpty() || MetaBytes.Num()>65536 || Guard.ContainsSecret(MetaBytes)) return Refused;
        const auto Bytes=EncodeQueueRecord(Item);
        if (Bytes.IsEmpty() || Bytes.Num()>QueueStorePrivate::RecordLimit(Policy)
            || DecodeQueueRecord(Bytes,Origin,Policy).HasError()) return Refused;
        const FString Pending=QueueStorePrivate::PendingPath(Directory,Item.Sequence,Item.QueueId);
#if WITH_DEV_AUTOMATION_TESTS
        auto Written=Hooks.IsValid() && Hooks->bFailPrepare
            ? Testing::WriteWithFault(Pending,Bytes,Testing::EFileFault::Flush,Hooks->BeforePrepareCleanup)
            : WriteNewFile(Pending,Bytes);
#else
        auto Written=WriteNewFile(Pending,Bytes);
#endif
#if WITH_DEV_AUTOMATION_TESTS
        if (Written.bOk && Hooks.IsValid()) Testing::WaitPersistenceBarrier(Hooks->AfterPrepare);
#endif
        return Written;
    },[Callback](FIoResult&& Result){ (*Callback)(FQueuePrepareResult{Result.bOk,Result.bCleanupOk}); },Reservation))
        (*Callback)(FQueuePrepareResult());
}
void VaRest::Http::FQueueStore::Commit(FString Directory,uint64 Sequence,FString QueueId,
    TUniqueFunction<void(FQueueCommitResult)>&& Done)
{
#if WITH_DEV_AUTOMATION_TESTS
    const auto Hooks=Testing::PersistenceHooksFor(Directory);
#endif
    const auto Result=MakeShared<FQueueCommitResult,ESPMode::ThreadSafe>();
    const auto Callback=MakeShared<TUniqueFunction<void(FQueueCommitResult)>,ESPMode::ThreadSafe>(MoveTemp(Done));
    if (!LaunchPersistence([Result,Directory=MoveTemp(Directory),Sequence,QueueId=MoveTemp(QueueId)
#if WITH_DEV_AUTOMATION_TESTS
        ,Hooks
#endif
    ]
    {
#if WITH_DEV_AUTOMATION_TESTS
        if (Hooks.IsValid()) Testing::WaitPersistenceBarrier(Hooks->BeforeRename);
#endif
        auto& Platform=FPlatformFileManager::Get().GetPlatformPhysical();
        const FString From=QueueStorePrivate::PendingPath(Directory,Sequence,QueueId);
        const FString To=FPaths::Combine(Directory,QueueFileName(Sequence,QueueId));

#if WITH_DEV_AUTOMATION_TESTS
        const bool bInjectedFailure=Hooks.IsValid() && Hooks->bFailCommit;
#else
        const bool bInjectedFailure=false;
#endif
        const FString NativeFrom=PersistencePathForIo(From),NativeTo=PersistencePathForIo(To);
        Result->bCommitted=!bInjectedFailure && !NativeFrom.IsEmpty() && !NativeTo.IsEmpty()
            && InspectPersistencePath(Directory).Kind==EPathKind::Directory
            && InspectPersistencePath(From).Kind==EPathKind::Regular
            && InspectPersistencePath(To).Kind==EPathKind::Missing
            && Platform.MoveFile(*NativeTo,*NativeFrom);
        Result->bCleanupOk=Result->bCommitted || VaRest::Http::DeleteFile(From).bOk;
        FIoResult Finished; Finished.bOk=true; return Finished;
    },[Result,Callback](FIoResult&&){ (*Callback)(*Result); },0,true))
        (*Callback)(FQueueCommitResult());
}
void VaRest::Http::FQueueStore::Abort(FString Directory,uint64 Sequence,FString QueueId,TUniqueFunction<void(bool)>&& Done)
{
    QueueStorePrivate::AsyncBool([Path=QueueStorePrivate::PendingPath(Directory,Sequence,QueueId)]
        { return VaRest::Http::DeleteFile(Path); },MoveTemp(Done),0,true);
}
void VaRest::Http::FQueueStore::Retire(FString Directory,uint64 Sequence,FString QueueId,
    TUniqueFunction<void(bool)>&& Done)
{
	RetireImpl(MoveTemp(Directory),Sequence,MoveTemp(QueueId),MoveTemp(Done),false);
}
bool VaRest::Http::FQueueStore::RetireOwned(FString Directory,uint64 Sequence,FString QueueId,
	TUniqueFunction<void(bool)>&& Done)
{
	return RetireImpl(MoveTemp(Directory),Sequence,MoveTemp(QueueId),MoveTemp(Done),true);
}
bool VaRest::Http::FQueueStore::RetireImpl(FString Directory,uint64 Sequence,FString QueueId,
	TUniqueFunction<void(bool)>&& Done,bool bControlDuringStop)
{
    return QueueStorePrivate::AsyncBool([Directory=MoveTemp(Directory),Sequence,QueueId=MoveTemp(QueueId)]
    {
        auto& Platform=FPlatformFileManager::Get().GetPlatformPhysical();
        const FString From=FPaths::Combine(Directory,QueueFileName(Sequence,QueueId));
        const FString Tombstone=FPaths::ChangeExtension(From,TEXT("done"));
        FIoResult Result;
        const FString NativeFrom=PersistencePathForIo(From),NativeTo=PersistencePathForIo(Tombstone);
        if (NativeFrom.IsEmpty() || NativeTo.IsEmpty()) return Result;
        const auto Parent=InspectPersistencePath(Directory);
        if (Parent.Kind!=EPathKind::Directory && Parent.Kind!=EPathKind::Missing) return Result;
        const auto Source=InspectPersistencePath(From),Retired=InspectPersistencePath(Tombstone);
        if ((Source.Kind!=EPathKind::Regular && Source.Kind!=EPathKind::Missing)
            || (Retired.Kind!=EPathKind::Regular && Retired.Kind!=EPathKind::Missing)) return Result;
        if (Retired.Kind==EPathKind::Regular)
            Result.bOk=VaRest::Http::DeleteFile(From).bOk;
        else if (Source.Kind==EPathKind::Missing) Result.bOk=true;
        else Result.bOk=Platform.MoveFile(*NativeTo,*NativeFrom);
        if (Result.bOk && InspectPersistencePath(From).Kind==EPathKind::Missing)
            VaRest::Http::DeleteFile(Tombstone);
        return Result;
    },MoveTemp(Done),0,bControlDuringStop);
}
int64 VaRest::Http::QueueMetadataCharge(const FQueueItem& Item)
{
    int64 Bytes=sizeof(FQueueItem)+Item.QueueId.GetAllocatedSize()+Item.Scope.GetAllocatedSize()
        +Item.Verb.GetAllocatedSize()+Item.Url.GetAllocatedSize()+Item.IdempotencyKey.GetAllocatedSize()
        +Item.Tag.GetAllocatedSize()+Item.Endpoint.GetAllocatedSize()+Item.Body.ContentType.GetAllocatedSize()+Item.Headers.GetPairs().GetAllocatedSize();
    for (const auto& Header : Item.Headers) Bytes+=Header.Key.GetAllocatedSize()+Header.Value.GetAllocatedSize();
    Bytes+=Item.OriginalPartNames.GetAllocatedSize();
    for (const FString& Name : Item.OriginalPartNames) Bytes+=Name.GetAllocatedSize();
    return FMath::Max(Bytes,Item.MetadataCharge); // copying a DTO can compact arrays; never reduce its reserved charge
}
TOptional<int64> VaRest::Http::MeasureQueueAdmissionMetadata(const FQueueItem& BodyFreeItem,
    FStringView Origin,const FVaRestOfflinePolicy& Policy)
{
    check(BodyFreeItem.Body.Bytes.IsEmpty());
    const int64 LiveCharge=QueueMetadataCharge(BodyFreeItem);
    if (LiveCharge>256*1024) return {};
    const auto Encoded=EncodeQueueRecord(BodyFreeItem);
    if (Encoded.IsEmpty() || Encoded.Num()>65536) return {};
    auto Recovered=DecodeQueueRecord(Encoded,Origin,Policy,0);
    if (Recovered.HasError()) return {};
    const int64 Charge=FMath::Max(LiveCharge,QueueMetadataCharge(Recovered.GetValue()));
    return Charge<=256*1024 ? TOptional<int64>(Charge) : TOptional<int64>();
}
void VaRest::Http::FQueueStore::Load(FString Directory,FString Origin,FVaRestOfflinePolicy Policy,
    TUniqueFunction<void(FQueueLoad&&)>&& Done)
{
    auto Callback=MakeShared<TUniqueFunction<void(FQueueLoad&&)>,ESPMode::ThreadSafe>(MoveTemp(Done));
    FVaRestOfflinePolicy HardPolicy=Policy; HardPolicy.MaxItemBytes=32*1024*1024;
#if WITH_DEV_AUTOMATION_TESTS
    const auto Hooks=Testing::PersistenceHooksFor(Directory);
#endif
    const int64 Reservation=Policy.MaxBodyBytes+20*1024*1024
        +QueueStorePrivate::RecordLimit(HardPolicy);
    if (!LaunchPersistenceResult<FQueueLoad>([Directory=MoveTemp(Directory),Origin=MoveTemp(Origin),Policy,HardPolicy
#if WITH_DEV_AUTOMATION_TESTS
        ,Hooks
#endif
    ]
    {
#if WITH_DEV_AUTOMATION_TESTS
        if (Hooks.IsValid()) Testing::WaitPersistenceBarrier(Hooks->BeforeQueueLoad);
#endif
        FQueueLoad Result;
        auto Listing=ListFiles(Directory);
        if (!Listing.bOk) { Result.bOk=false; return Result; }
        struct FNameRecord { FString Name,Id; uint64 Sequence=0; };
        TArray<FNameRecord> Names;
        TMap<FString,int32> IdCounts; TMap<uint64,int32> SequenceCounts;
        TSet<FString> Retired;
        for (const FString& Name : Listing.Names)
        {
            uint64 Sequence=0; FString Id;
            if (QueueStorePrivate::FileIdentity(Name,Sequence,Id,TEXT(".done")))
            {
                const FString JsonName=QueueFileName(Sequence,Id); Retired.Add(JsonName);
                if (VaRest::Http::DeleteFile(FPaths::Combine(Directory,JsonName)).bOk)
                    VaRest::Http::DeleteFile(FPaths::Combine(Directory,Name));
            }
            else if (Name.EndsWith(TEXT(".pending")) || Name.Contains(TEXT(".part")))
            {
                if (!VaRest::Http::DeleteFile(FPaths::Combine(Directory,Name)).bOk) Result.bOk=false;
            }
        }
        for (FString& Name : Listing.Names)
        {
            FNameRecord Record;
            if (!QueueStorePrivate::FileIdentity(Name,Record.Sequence,Record.Id,TEXT(".json")) || Retired.Contains(Name)) continue;
            Record.Name=MoveTemp(Name);
            ++IdCounts.FindOrAdd(Record.Id); ++SequenceCounts.FindOrAdd(Record.Sequence);
            Names.Add(MoveTemp(Record));
        }
        Listing.Names.Empty(); // release discarded strings and the original array before reading any payload
        // Remove every conflicting name before any capacity refusal can stop payload recovery.
        Names.RemoveAll([&](const FNameRecord& Name)
        {
            if (IdCounts.FindChecked(Name.Id)<=1 && SequenceCounts.FindChecked(Name.Sequence)<=1) return false;
            Result.bOk=false; VaRest::Http::DeleteFile(FPaths::Combine(Directory,Name.Name)); return true;
        });
        Names.Sort([](const FNameRecord& Left,const FNameRecord& Right){ return Left.Sequence<Right.Sequence; });
        int64 BodyBytes=0,MetadataBytes=0;
        for (const FNameRecord& Name : Names)
        {
            const FString Path=FPaths::Combine(Directory,Name.Name);
            if (Result.Items.Num()>=Policy.MaxQueued)
            { Result.bOk=false; Result.bPolicyExceeded=true; break; }
            auto Read=ReadBoundedFile(Path,QueueStorePrivate::RecordLimit(HardPolicy));
            if (!Read.bOk) { Result.bOk=false; continue; } // IO failure is not proof of corruption
            bool bCapacityRefused=false;
            const int64 Available=FMath::Min<int64>(Policy.MaxItemBytes,Policy.MaxBodyBytes-BodyBytes);
            auto Parsed=DecodeQueueRecord(Read.Bytes,Origin,HardPolicy,Available,&bCapacityRefused);
            if (bCapacityRefused) { Result.bOk=false; Result.bPolicyExceeded=true; break; }
            if (Parsed.HasError() || Parsed.GetValue().Sequence!=Name.Sequence || Parsed.GetValue().QueueId!=Name.Id)
            { Result.bOk=false; VaRest::Http::DeleteFile(Path); continue; }
            FQueueItem Item=Parsed.StealValue();
            const int64 Charge=QueueMetadataCharge(Item);
            Result.PeakBodyBytes=FMath::Max(Result.PeakBodyBytes,BodyBytes+Item.Body.Bytes.Num());
            Result.PeakMetadataBytes=FMath::Max(Result.PeakMetadataBytes,MetadataBytes+Charge);
            if (Charge>256*1024 || MetadataBytes>10*1024*1024-Charge)
            { Result.bOk=false; Result.bPolicyExceeded=true; break; }
            Item.MetadataCharge=Charge;
            BodyBytes+=Item.Body.Bytes.Num(); MetadataBytes+=Charge;
            Result.NextSequence=Item.Sequence==MAX_uint64 ? 0 : Item.Sequence+1;
            Result.Items.Add(MoveTemp(Item));
        }
        if (!Result.bOk) Result.Items.Reset(); // never expose or replay a partial failed recovery
        return Result;
    },[Callback](FQueueLoad&& Result){ (*Callback)(MoveTemp(Result)); },Reservation))
    { FQueueLoad Failed; Failed.bOk=false; (*Callback)(MoveTemp(Failed)); }
}
