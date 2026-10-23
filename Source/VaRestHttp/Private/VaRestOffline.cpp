// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestOffline.h"
#include "VaRestOfflineInternal.h"
#include "VaRestQueueStore.h"
#include "VaRestClientState.h"
#include "VaRestExchangeState.h"
#include "VaRestIdentity.h"
#include "VaRestPersistence.h"
#include "VaRestPersistenceWait.h"
#include "VaRestPersistenceGuard.h"
#include "VaRestRequestSpec.h"
#include "VaRestRetryInternal.h"
#include "VaRestClassify.h"
#include "VaRestTracePoints.h"
#include "VaRestHttpTesting.h"
#include "VaRestHttpStatsInternal.h"
#include "VaRestJson.h"
#include "VaRestUrl.h"
#include "Misc/Paths.h"
#include "Misc/Crc.h"

namespace VaRest::Http::OfflinePrivate
{
    struct FRootKeys : TDefaultMapHashableKeyFuncs<FString,TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe>,false>
    {
        static bool Matches(const FString& Left,const FString& Right)
        {
#if PLATFORM_LINUX
            return Left.Equals(Right,ESearchCase::CaseSensitive);
#else
            return Left==Right;
#endif
        }
        static uint32 GetKeyHash(const FString& Key)
        {
#if PLATFORM_LINUX
            return FCrc::StrCrc32(*Key);
#else
            return GetTypeHash(Key);
#endif
        }
    };
    using FRegistry=TMap<FString,TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe>,FDefaultSetAllocator,FRootKeys>;
    FRegistry& Registry()
    { static auto* Values=new FRegistry(); return *Values; }
    bool PlainName(FStringView Name)
    {
        for (TCHAR Character : Name) if (Character<32 || Character==127) return false;
        return true;
    }
    bool SamePolicy(const FVaRestOfflinePolicy& A,const FVaRestOfflinePolicy& B)
    {
        return A.MaxQueued==B.MaxQueued && A.MaxAgeSeconds==B.MaxAgeSeconds
            && A.MaxItemBytes==B.MaxItemBytes && A.MaxBodyBytes==B.MaxBodyBytes;
    }
    bool ValidPolicy(const FVaRestOfflinePolicy& Policy)
    {
        return Policy.MaxQueued>0 && Policy.MaxQueued<=4096 && FMath::IsFinite(Policy.MaxAgeSeconds)
            && Policy.MaxAgeSeconds>0 && Policy.MaxAgeSeconds<=315360000
            && Policy.MaxItemBytes>0 && Policy.MaxItemBytes<=32*1024*1024
            && Policy.MaxBodyBytes>=Policy.MaxItemBytes && Policy.MaxBodyBytes<=1024LL*1024*1024;
    }
}
bool VaRest::Http::QueueScope(const FExchangeState& Ex,FString& OutScope)
{ return RequestIdentityScope(*Ex.Config,Ex.State->GetSessionData(),Ex.AuthMode,Ex.RequestHeaders,OutScope); }
bool VaRest::Http::QueueIdentityStillCurrent(const FExchangeState& Ex)
{
    if (Ex.QueueIdentityScope.IsEmpty()) return false;
    const auto Current=Ex.State->GetConfig();
    if (Ex.AuthMode!=EVaRestAuthMode::None && Current->Auth!=Ex.Config->Auth) return false;
    FString Scope,Origin;
    return CanonicalOrigin(Ex.Url,Origin) && Origin==Ex.QueueAnchorOrigin
        && SameOrigin(Current->BaseUrl,Ex.Url)
        && RequestIdentityScope(*Current,Ex.State->GetSessionData(),Ex.AuthMode,Ex.QueueScopeHeaders,Scope)
        && Scope==Ex.QueueIdentityScope;
}
TSharedPtr<VaRest::Http::FOfflineQueue,ESPMode::ThreadSafe> VaRest::Http::FOfflineQueue::Join(
    const TSharedRef<FClientState,ESPMode::ThreadSafe>& Client,FString& OutConfigError)
{
    LLM_SCOPE_BYTAG(VaRest);
    check(IsInGameThread()); OutConfigError.Reset();
    const auto Config=Client->GetConfig();
    if (Config->QueueName.IsEmpty()) return nullptr;
    FString Canonical;
    if (FClientState::IsShuttingDown() || !PersistenceAccepting() || !Config->StorageConfigError.IsEmpty()
        || Config->QueueRoot.Len()>4096 || !OfflinePrivate::PlainName(Config->QueueName)
        || !OfflinePrivate::ValidPolicy(Config->OfflinePolicy) || Config->QueueName.Len()>1024
        || !IsValidHeaderValue(Config->QueueName) || !CanonicalOrigin(Config->BaseUrl,Canonical)
        || !Config->Connectivity.IsValid()) { OutConfigError=TEXT("QueueConfig"); return nullptr; }
    const FString Path=QueueDirectory(Config->QueueRoot,Config->QueueName);
    if (Path.Len()>4096 || Path.GetAllocatedSize()>8192*sizeof(TCHAR))
    { OutConfigError=TEXT("QueueConfig"); return nullptr; }
    const FString Category=Config->Auth.IsValid() ? Config->Auth->GetAuthName().ToString().ToLower() : FString(TEXT("None"));
    const FName Contract=Config->Auth.IsValid() ? Config->Auth->GetIdentityContractName() : NAME_None;
    auto& Registry=OfflinePrivate::Registry();
    for (auto It=Registry.CreateIterator();It;++It) if (!It.Value().IsValid()) It.RemoveCurrent();
    auto Queue=Registry.FindRef(Path).Pin();
    if (Queue.IsValid())
    {
        if (Queue->bStopped || Queue->Origin!=Canonical || Queue->Connectivity.Pin()!=Config->Connectivity
            || !OfflinePrivate::SamePolicy(Queue->Policy,Config->OfflinePolicy) || Queue->AuthCategory!=Category
            || Queue->IdentityContract!=Contract
            || (Contract.IsNone() && Queue->IdentityProvider.Pin()!=Config->Auth))
        { OutConfigError=TEXT("QueueConfig"); return nullptr; }
    }
    else
    {
        Queue=MakeShared<FOfflineQueue,ESPMode::ThreadSafe>();
        Queue->Directory=Path; Queue->Origin=Canonical; Queue->Policy=Config->OfflinePolicy;
        Queue->Connectivity=Config->Connectivity; Queue->AuthCategory=Category;
        Queue->IdentityContract=Contract; Queue->IdentityProvider=Config->Auth;
        Queue->QueueClock=Config->Clock; Queue->MonotonicOrigin=Config->Clock->Now(); Queue->UtcOrigin=FDateTime::UtcNow();
        Registry.Add(Path,Queue);
        Queue->Load();
        const TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe> Weak(Queue);
        Queue->ConnectivityChanged=Config->Connectivity->OnChanged().AddLambda([Weak](bool bOnline)
        { if (bOnline) if (const auto Pinned=Weak.Pin()) Pinned->ScheduleReplay(); });
    }
    Queue->Members.Add(Client->GetClientId(),Client);
    Queue->ScheduleReplay();
    return Queue;
}
void VaRest::Http::FOfflineQueue::Leave(uint64 ClientId) { check(IsInGameThread()); Members.Remove(ClientId); }
int32 VaRest::Http::FOfflineQueue::Count() const { check(IsInGameThread()); return Items.Num()+ReservedCount+AfterLoad.Num(); }
FDateTime VaRest::Http::FOfflineQueue::UtcNow() const
{ return UtcOrigin+FTimespan::FromSeconds(FMath::Max(0.0,QueueClock->Now()-MonotonicOrigin)); }
void VaRest::Http::FOfflineQueue::Load()
{
    LLM_SCOPE_BYTAG(VaRest);
    FQueueStore::Load(Directory,Origin,Policy,[Self=AsShared()](FQueueLoad&& Loaded)
    {
        LLM_SCOPE_BYTAG(VaRest);
        if (Self->bStopped) return;
        Self->Items=MoveTemp(Loaded.Items); Self->NextSequence=Loaded.NextSequence;
        Self->bStorageFailed=!Loaded.bOk; Self->CommittedBodyBytes=0; Self->CommittedMetadataBytes=0;
        for (const auto& Item : Self->Items)
        { Self->CommittedBodyBytes+=Item.Body.Bytes.Num(); Self->CommittedMetadataBytes+=QueueMetadataCharge(Item); }
        // The load's worker/result reservation remains charged until this callback returns.
        // Admit prepares only on the subsequent owned turn, after that reservation releases.
        if (!PostPersistenceContinuation([Self]{ Self->DrainLoadedWaiters(); }))
        {
            Self->bStorageFailed=true;
            Self->DrainLoadedWaiters(); // terminal refusals only; never start Prepare
        }
    });
}

void VaRest::Http::FOfflineQueue::DrainLoadedWaiters()
{
    LLM_SCOPE_BYTAG(VaRest);
    // Keep pending admissions visible and append reentrant work behind every older waiter.
    while (!bStopped && !AfterLoad.IsEmpty())
    {
        auto Start=MoveTemp(AfterLoad[0]);
        AfterLoad.RemoveAt(0);
        Start();
    }
    if (bStopped) return;
    bLoaded=true;
    ScheduleReplay();
}
bool VaRest::Http::FOfflineQueue::Reserve(int64 BodyBytes,int64 MetadataBytes,FString& OutError)
{
    check(IsInGameThread());
    if (bStopped || BodyBytes < 0 || BodyBytes > Policy.MaxItemBytes || BodyBytes > Policy.MaxBodyBytes)
    { OutError = TEXT("QueueFull"); return false; }
    if (MetadataBytes<0 || MetadataBytes>256*1024
        || CommittedMetadataBytes+ReservedMetadataBytes>10*1024*1024-MetadataBytes
        || Items.Num() + ReservedCount >= Policy.MaxQueued
        || CommittedBodyBytes + ReservedBodyBytes > Policy.MaxBodyBytes - BodyBytes)
    { OutError = TEXT("QueueFull"); return false; }
    ++ReservedCount;
    ReservedBodyBytes += BodyBytes; ReservedMetadataBytes+=MetadataBytes;
    return true;
}
void VaRest::Http::FOfflineQueue::ReleaseReservation(int64 BodyBytes,int64 MetadataBytes)
{
    check(IsInGameThread());
    check(ReservedCount > 0 && ReservedBodyBytes >= BodyBytes && ReservedMetadataBytes>=MetadataBytes);
    --ReservedCount;
    ReservedBodyBytes -= BodyBytes; ReservedMetadataBytes-=MetadataBytes;
}
FString VaRest::Http::QueueDirectory(FStringView Root, FStringView Name)
{
    return FPaths::Combine(FString(Root), HashFields(TEXT("VaRest/queue-name/v1"), TArray<FString>{FString(Name)}));
}

VaRest::Http::FOfflineQueue::~FOfflineQueue()
{
    check(IsInGameThread());
    if (const auto Reachability=Connectivity.Pin()) Reachability->OnChanged().Remove(ConnectivityChanged);
    if (QueueClock.IsValid()) QueueClock->ClearTimer(ReplayRetryTimer);
}
void VaRest::Http::FOfflineQueue::StopForShutdown()
{
    check(IsInGameThread()); bStorageFailed=true; bStopped=true;
    AfterLoad.Reset(); WaitingBodyBytes=0; FlushWaiters.Reset();
    if (QueueClock.IsValid()) QueueClock->ClearTimer(ReplayRetryTimer);
}
void VaRest::Http::FOfflineQueue::StopAll()
{
    check(IsInGameThread());
    TArray<TSharedPtr<FOfflineQueue,ESPMode::ThreadSafe>> Alive;
    for (const auto& Pair : OfflinePrivate::Registry()) if (const auto Queue=Pair.Value.Pin()) Alive.Add(Queue);
    for (const auto& Queue : Alive) Queue->StopForShutdown();
}

FVaRestAttemptOutcome VaRest::Http::LocalFailure(FName Detail)
{
    FVaRestAttemptOutcome Result; Result.Failure=EVaRestTransportFailure::Other;
    Result.FailureDetail=Detail; Result.FailureMessage=TEXT("local queue operation failed"); return Result;
}

namespace VaRest::Http
{
struct FQueueAdmissionToken
{
    TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe> Owner;
    uint64 Ticket;
    FQueueAdmissionToken(const TSharedRef<FOfflineQueue,ESPMode::ThreadSafe>& InOwner,uint64 InTicket)
        : Owner(InOwner),Ticket(InTicket) {}
    ~FQueueAdmissionToken()
    { check(IsInGameThread()); if (const auto Queue=Owner.Pin()) Queue->FinishAdmission(Ticket); }
};

class FQueueAdmissionJob : public TSharedFromThis<FQueueAdmissionJob,ESPMode::ThreadSafe>
{
public:
    FQueueAdmissionJob(FVaRestExchangeRef InExchange,TSharedRef<FPersistenceWait,ESPMode::ThreadSafe> InWait,
        FQueueItem InItem,TSharedRef<FOfflineQueue,ESPMode::ThreadSafe> InQueue,int64 InReservedBytes,int64 InReservedMetadata)
        : Exchange(MoveTemp(InExchange)),Wait(MoveTemp(InWait)),Item(MoveTemp(InItem)),Queue(MoveTemp(InQueue)),ReservedBytes(InReservedBytes),ReservedMetadata(InReservedMetadata) {}
    ~FQueueAdmissionJob()
    {
        // Stop may discard Prepare's ordinary handback. Release its bounded accounting on GT,
        // conservatively leaving storage unavailable until normal recovery cleans any pending bytes.
        if (bReserved) ResolveCleanup(false);
    }
    void ResolveCleanup(bool bClean)
    {
        if (!bClean) { Queue->bStorageFailed=true; Queue->bFlushFailed=true; }
        check(bReserved); bReserved=false;
        Queue->ReleaseReservation(ReservedBytes,ReservedMetadata);
    }
    void AbortPrepared()
    {
        LLM_SCOPE_BYTAG(VaRest);
        // Keep the job, admission ticket and reservation until the actual owned cleanup result.
        FQueueStore::Abort(Queue->GetDirectory(),Item.Sequence,Item.QueueId,
            [Self=AsShared()](bool bClean){ Self->ResolveCleanup(bClean); });
    }
    void Prepared(bool bOk);
    FVaRestExchangeRef Exchange;
    TSharedRef<FPersistenceWait,ESPMode::ThreadSafe> Wait;
    FQueueItem Item;
    TSharedRef<FOfflineQueue,ESPMode::ThreadSafe> Queue;
    int64 ReservedBytes,ReservedMetadata;
    bool bPrepared=false,bReserved=true;
    TSharedPtr<FQueueAdmissionToken,ESPMode::ThreadSafe> Admission;
};

void GuardReplay(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done);
class FOfflineStage final : public IVaRestStage
{
public:
    virtual FName GetStageName() const override { return TEXT("Offline"); }
    virtual void Process(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& OnOutcome) override;
};
void FOfflineStage::Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next,
    FVaRestOutcomeCallback&& OnOutcome)
{
    LLM_SCOPE_BYTAG(VaRest);
    auto& Ex = Internals(*Exchange);
    if (!Ex.QueueTerminalDetail.IsNone())
    {
        if (Ex.QueueTerminalDetail==TEXT("QueueExpired"))
        {
            FVaRestTraceNote Note; Note.Kind=EVaRestTraceNoteKind::QueueExpired; Note.QueueId=Ex.QueueReplayOf.QueueId;
            TracePoints::Note(Ex,MoveTemp(Note));
        }
        OnOutcome(LocalFailure(Ex.QueueTerminalDetail)); return;
    }
    if (!Ex.QueueReplayOf.QueueId.IsEmpty()) { GuardReplay(Exchange,Next,MoveTemp(OnOutcome)); return; }
    if (Ex.bIsAuthRefresh || Ex.bConnectivityProbe || !Ex.bQueueWhenOffline)
    {
        Next.Proceed(Exchange, MoveTemp(OnOutcome));
        return;
    }
    if (!QueueIdentityStillCurrent(Ex)) { OnOutcome(LocalFailure(TEXT("QueueIdentity"))); return; }
    const auto Queue = Ex.Queue; // Send/Resolve snapshot, not current client's later queue setting
    if (!Ex.Config->Connectivity->IsOnline())
    {
        Queue->Enqueue(Exchange, MoveTemp(OnOutcome));
        return;
    }
    Next.Proceed(Exchange, [Exchange, Queue, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
    {
        const auto Kind = ClassifyAttempt(Outcome).Kind;
        if (Kind == EVaRestErrorKind::Connection || Kind == EVaRestErrorKind::Offline)
            Queue->Enqueue(Exchange, MoveTemp(Done));
        else Done(MoveTemp(Outcome));
    });
}

}
void VaRest::Http::GuardReplay(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done)
{
    LLM_SCOPE_BYTAG(VaRest);
    auto& Ex=Internals(*Exchange);
    auto Wait=FPersistenceWait::Begin(Exchange,MoveTemp(Done));
    if (!Wait->IsPending()) return;
    FPersistenceGuard Guard=*Ex.PersistenceGuard; Guard.UnionWith(*Ex.State->GetConfig()->PersistenceGuard);
    Guard.AbortSignal=Ex.PersistenceAbort;
    const int64 Reservation=PersistenceGuardReservation(Ex.Body.Kind,Ex.Body.ContentType,Ex.Body.Bytes.Num());
    const bool bAdmitted=LaunchPersistence([Guard=MoveTemp(Guard),Body=Ex.Body,Headers=Ex.RequestHeaders,
        Url=Ex.Url,Tag=Ex.Tag.ToString(),Verb=Ex.Verb]() mutable
    {
        FIoResult Result;
        for (const auto& Part : Body.PartDescriptors)
            if (Guard.Rules.ShouldRedact(Part.Name,EVaRestRedactKeyKind::Part)) return Result;
        FVaRestJson Metadata=FVaRestJson::Object();
        Metadata.Set(TEXT("url"),Url); Metadata.Set(TEXT("tag"),Tag); Metadata.Set(TEXT("verb"),Verb);
        Metadata.Set(TEXT("contentType"),Body.ContentType);
        FVaRestJson HeaderJson=FVaRestJson::Array();
        for (const auto& Header : Headers) HeaderJson.Add(FVaRestJson::Array(Header.Key,Header.Value));
        Metadata.Set(TEXT("headers"),HeaderJson);
        TArray<uint8> MetadataBytes;
        Result.bOk=Guard.AllowsUrl(Url) && Guard.AllowsBody(Body.Kind,Body.ContentType,Body.Bytes)
            && Guard.SafeHeaders(Headers).Num()==Headers.Num() && Metadata.TryToUtf8(MetadataBytes).HasValue()
            && MetadataBytes.Num()<=65536 && !Guard.ContainsSecret(MetadataBytes);
        return Result;
    },[Exchange,Next,Wait](FIoResult&& Result) mutable
    {
        LLM_SCOPE_BYTAG(VaRest);
        if (!Wait->IsPending()) return;
        if (!Result.bOk) { Wait->Finish(LocalFailure(TEXT("PersistenceCredentials"))); return; }
        FVaRestOutcomeCallback Resume=Wait->Resume();
        if (Resume) Next.Proceed(Exchange,MoveTemp(Resume));
    },Reservation);
    if (!bAdmitted) Wait->Finish(LocalFailure(TEXT("PersistenceStopped")));
}
FVaRestStageRef VaRest::Http::MakeOfflineStage()
{ return MakeShared<FOfflineStage,ESPMode::ThreadSafe>(); }

TArray<uint8> VaRest::Http::CopyQueueAdmissionBytes(TConstArrayView<uint8> Bytes)
{
#if WITH_DEV_AUTOMATION_TESTS
    Testing::Counters().QueueAdmissionBytesCopied.fetch_add(Bytes.Num());
#endif
    return TArray<uint8>(Bytes.GetData(),Bytes.Num());
}

void VaRest::Http::FOfflineQueue::Enqueue(const FVaRestExchangeRef& Exchange,FVaRestOutcomeCallback&& Done)
{
    LLM_SCOPE_BYTAG(VaRest);
    auto Wait=FPersistenceWait::Begin(Exchange,MoveTemp(Done));
    if (!Wait->IsPending()) return;
    if (LastAdmission==MAX_uint64) { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
    const uint64 Ticket=++LastAdmission; ActiveAdmissions.Add(Ticket);
    const auto Admission=MakeShared<FQueueAdmissionToken,ESPMode::ThreadSafe>(AsShared(),Ticket);
    const auto Start=[Self=AsShared(),Exchange,Wait,Admission]
    {
        LLM_SCOPE_BYTAG(VaRest);
        if (!Wait->CanHandoff()) return;
        auto& Ex=Internals(*Exchange);
        FString Scope,Error;
        if (Self->bStopped || Self->bStorageFailed || Self->NextSequence==0)
        { Wait->Finish(LocalFailure(TEXT("QueueWriteFailed"))); return; }
        if (!QueueIdentityStillCurrent(Ex)) { Wait->Finish(LocalFailure(TEXT("QueueIdentity"))); return; }
        Scope=Ex.QueueIdentityScope;
        if (!SameOrigin(Self->Origin,Ex.Url) || !SameOrigin(Ex.State->GetConfig()->BaseUrl,Ex.Url))
        { Wait->Finish(LocalFailure(TEXT("QueueOrigin"))); return; }
        FPersistenceGuard Guard=*Ex.PersistenceGuard;
        Guard.UnionWith(*Ex.State->GetConfig()->PersistenceGuard); Guard.AbortSignal=Ex.PersistenceAbort;
        const int64 BodyBytes=Ex.Body.Bytes.Num();
        if (BodyBytes>Self->Policy.MaxItemBytes || BodyBytes>Self->Policy.MaxBodyBytes
            || Ex.RequestHeaders.Num()>128 || Ex.Url.Len()>8192 || Ex.Verb.Len()>8192 || Ex.Tag.GetStringLength()>256
            || Ex.RequestHeaders.Get(TEXT("Content-Type")).Len()>1024 || Ex.Body.PartDescriptors.Num()>128)
        { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
        int64 HeaderChars=0;
        for (const auto& Header : Ex.RequestHeaders)
        {
            HeaderChars+=Header.Key.Len()+Header.Value.Len();
            if (Header.Key.Len()>256 || Header.Value.Len()>8192 || HeaderChars>32768)
            { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
        }
        FQueueItem Item;
        Item.QueueId=FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);
        Item.Sequence=Self->NextSequence;
        Self->NextSequence=Self->NextSequence==MAX_uint64 ? 0 : Self->NextSequence+1;
        Item.OriginalRun=QueueRunId(); Item.OriginalRequestId=Ex.RequestId; Item.CreatedUtc=Self->UtcNow();
        Item.Scope=MoveTemp(Scope); Item.Verb=Ex.Verb; Item.Url=Ex.Url; Item.Headers=Ex.RequestHeaders;
        if (Ex.bCacheConditional) { Item.Headers.Remove(TEXT("If-None-Match")); Item.Headers.Remove(TEXT("If-Modified-Since")); }
        if (Ex.Body.PartDescriptors.Num()>128) { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
        int64 PartChars=0;
        for (const auto& Part : Ex.Body.PartDescriptors)
        {
            PartChars+=Part.Name.Len();
            if (PartChars>16384) { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
            Item.OriginalPartNames.Add(Part.Name);
        }
        Item.Body=FVaRestRequestBody::FromBytes(TArray<uint8>(),Ex.Body.Kind,Ex.RequestHeaders.Get(TEXT("Content-Type")));
        Item.IdempotencyKey=Ex.RequestHeaders.Get(TEXT("Idempotency-Key"));
        Item.Tag=Ex.Tag.IsNone() ? FString() : Ex.Tag.ToString(); Item.AuthMode=Ex.AuthMode; Item.bIdempotent=Ex.bIdempotent;
        if (Item.Tag.Len()>256) { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
        const auto Measured=MeasureQueueAdmissionMetadata(Item,Self->Origin,Self->Policy);
        if (!Measured.IsSet()) { Wait->Finish(LocalFailure(TEXT("QueueWriteFailed"))); return; }
        const int64 MetadataBytes=Measured.GetValue(); Item.MetadataCharge=MetadataBytes;
        if (!Self->Reserve(BodyBytes,MetadataBytes,Error)) { Wait->Finish(LocalFailure(FName(*Error))); return; }
        // No payload copy occurs before the count/body/metadata reservation succeeds.
        TArray<uint8> OwnedBytes=CopyQueueAdmissionBytes(Ex.Body.Bytes);
        Item.Body=FVaRestRequestBody::FromBytes(MoveTemp(OwnedBytes),Ex.Body.Kind,Item.Body.ContentType);
        const auto Job=MakeShared<FQueueAdmissionJob,ESPMode::ThreadSafe>(Exchange,Wait,MoveTemp(Item),Self,BodyBytes,MetadataBytes);
        FQueueItem WorkerItem=Job->Item; // bounded second body copy, after the same successful reservation
#if WITH_DEV_AUTOMATION_TESTS
        Testing::Counters().QueueAdmissionBytesCopied.fetch_add(BodyBytes);
#endif
        Job->Admission=Admission;
        FQueueStore::Prepare(Self->Directory,Self->Origin,Self->Policy,MoveTemp(WorkerItem),MoveTemp(Guard),
            [Job](bool bOk){ Job->Prepared(bOk); });
    };
    if (bLoaded) Start();
    else if (AfterLoad.Num()>=Policy.MaxQueued) Wait->Finish(LocalFailure(TEXT("QueueFull")));
    else
    {
        const int64 WaitingBytes=Internals(*Exchange).Body.Bytes.Num();
        if (WaitingBytes>Policy.MaxItemBytes || WaitingBodyBytes>Policy.MaxBodyBytes-WaitingBytes)
        { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
        WaitingBodyBytes+=WaitingBytes;
        AfterLoad.Add([Self=AsShared(),Start,WaitingBytes]
        { Self->WaitingBodyBytes-=WaitingBytes; Start(); });
    }
}
void VaRest::Http::FOfflineQueue::AcceptCommitted(FQueueItem Item)
{
    CommittedBodyBytes+=Item.Body.Bytes.Num(); CommittedMetadataBytes+=QueueMetadataCharge(Item); Items.Add(MoveTemp(Item));
    Items.Sort([](const FQueueItem& Left,const FQueueItem& Right){ return Left.Sequence<Right.Sequence; });
}

void VaRest::Http::FQueueAdmissionJob::Prepared(bool bOk)
{
    LLM_SCOPE_BYTAG(VaRest);
    auto& Ex=Internals(*Exchange);
    if (!bOk)
    {
        Queue->bFlushFailed=true;
        ResolveCleanup(true);
        if (Wait->IsPending()) Wait->Finish(LocalFailure(TEXT("QueueWriteFailed")));
        return;
    }
    bPrepared=true;
    const auto Current=Ex.State->GetConfig();
    const bool bValidIdentity=QueueIdentityStillCurrent(Ex) && Item.Scope==Ex.QueueIdentityScope;
    const bool bValidOrigin=SameOrigin(Current->BaseUrl,Item.Url);
    const bool bDeadline=Ex.DeadlineAt>0 && Ex.Config->Clock->Now()>=Ex.DeadlineAt-DeadlineTimerSlack;
    if (!Wait->IsPending() || Ex.IsCancelRequested() || IsOwnerGone(Ex) || bDeadline
        || !bValidIdentity || !bValidOrigin || FClientState::IsShuttingDown())
    {
        EQueueAdmission Expected=EQueueAdmission::Open;
        Ex.QueueAdmission.compare_exchange_strong(Expected,EQueueAdmission::Aborted,std::memory_order_acq_rel);
        AbortPrepared();
        if (Wait->IsPending())
        {
            if (bDeadline) Wait->Finish(MakeDeadlineOutcome());
            else if (!bValidIdentity) Wait->Finish(LocalFailure(TEXT("QueueIdentity")));
            else if (!bValidOrigin) Wait->Finish(LocalFailure(TEXT("QueueOrigin")));
            else Wait->Finish(FVaRestAttemptOutcome::Canceled());
        }
        return;
    }
    EQueueAdmission Expected=EQueueAdmission::Open;
    if (!Ex.QueueAdmission.compare_exchange_strong(Expected,EQueueAdmission::Committing,std::memory_order_acq_rel))
    {
        AbortPrepared();
        if (Wait->IsPending()) Wait->Finish(FVaRestAttemptOutcome::Canceled());
        return;
    }
    // The deadline/interrupt are disarmed at the same acceptance point; no yield before Resume.
    FVaRestOutcomeCallback Accepted=Wait->TakeCompletion(); // already past the shared commit CAS; no later deadline override
    FQueueStore::Commit(Queue->GetDirectory(),Item.Sequence,Item.QueueId,
        [Self=AsShared(),Done=MoveTemp(Accepted)](FQueueCommitResult Result) mutable
        {
            LLM_SCOPE_BYTAG(VaRest);
            auto& AcceptedEx=Internals(*Self->Exchange);
            Self->ResolveCleanup(Result.bCleanupOk);
            if (!Result.bCommitted)
            {
                Self->Queue->bFlushFailed=true;
                Done(LocalFailure(TEXT("QueueWriteFailed")));
                return;
            }
            Self->Queue->AcceptCommitted(Self->Item);
            AcceptedEx.bQueueCommitted=true;
            if (!AcceptedEx.bFinished && !AcceptedEx.bCompletionQueued && AcceptedEx.Phase==EVaRestExchangePhase::InChain)
            {
                FVaRestTraceNote Note; Note.Kind=EVaRestTraceNoteKind::Queued;
                Note.QueueId=Self->Item.QueueId; Note.Reason=TEXT("Offline");
                TracePoints::Note(AcceptedEx,MoveTemp(Note));
            }
            FVaRestAttemptOutcome Outcome; Outcome.bQueued=true; Outcome.QueueId=Self->Item.QueueId;
            Done(MoveTemp(Outcome));
            Self->Queue->ScheduleReplay();
        });
}

TSharedPtr<VaRest::Http::FClientState,ESPMode::ThreadSafe> VaRest::Http::FOfflineQueue::SelectOwner() const
{
    TArray<uint64> Ids; Members.GetKeys(Ids); Ids.Sort();
    for (uint64 Id : Ids)
        if (const auto Candidate=Members.FindChecked(Id).Pin())
            if (Candidate->Queue.Get()==this) return Candidate;
    return nullptr;
}
void VaRest::Http::FOfflineQueue::ScheduleReplay()
{
    LLM_SCOPE_BYTAG(VaRest);
    if (bStopped || bReplayScheduled || !bLoaded || bStorageFailed || Items.IsEmpty() || FClientState::IsShuttingDown()) return;
    bReplayScheduled=true;
    const TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe> Weak(AsShared());
    if (!PostPersistenceContinuation([Weak]
    {
        if (const auto Self=Weak.Pin()) { Self->bReplayScheduled=false; Self->TryReplay(false); }
    })) { bReplayScheduled=false; bStorageFailed=true; }
}
bool VaRest::Http::FOfflineQueue::TryReplay(bool bProbe)
{
    LLM_SCOPE_BYTAG(VaRest);
    check(IsInGameThread());
    if (bStopped || !bLoaded || bStorageFailed || bReplayActive || Items.IsEmpty() || FClientState::IsShuttingDown()
        || QueueClock->Now()<ReplayNotBefore) return false;
    const auto Reachability=Connectivity.Pin();
    const auto Owner=SelectOwner();
    if (!Reachability.IsValid() || !Owner.IsValid() || (!bProbe && !Reachability->IsOnline())) return false;
    bReplayActive=true; bProbeReplay=bProbe;
    const FQueueItem Item=Items[0];
    FName Terminal;
    const auto Current=Owner->GetConfig();
    FString Scope;
    const double Age=(UtcNow()-Item.CreatedUtc).GetTotalSeconds();
    if (Age<0 || Age>=Policy.MaxAgeSeconds) Terminal=TEXT("QueueExpired");
    else if (!SameOrigin(Current->BaseUrl,Item.Url)) Terminal=TEXT("QueueOrigin");
    else if (!RequestIdentityScope(*Current,Owner->GetSessionData(),Item.AuthMode,Item.Headers,Scope) || Scope!=Item.Scope)
        Terminal=TEXT("QueueIdentity");
    FRequestSpecAccess::SendQueued(Owner.ToSharedRef(),Item,bProbe,
        [Self=AsShared(),Owner,Item,Terminal](const TVaRestResult<TArray<uint8>>& Reply)
        {
            FVaRestQueuedResult Result; Result.QueueId=Item.QueueId;
            Result.Tag=Item.Tag.IsEmpty() ? NAME_None : FName(*Item.Tag);
            Result.bSuccess=!Reply.Error.IsError(); Result.Response=Reply.Response; Result.Error=Reply.Error;
            if (Reply.Value.IsSet()) Result.Response.Body=Reply.Value.GetValue();
            const bool bKeep=Reply.Error.Kind==EVaRestErrorKind::Connection || Reply.Error.Kind==EVaRestErrorKind::Offline;
            // Expiry is terminal Offline; a network Offline must remain pending.
            Self->ReplayFinished(Item.QueueId,MoveTemp(Result),bKeep && Terminal!=TEXT("QueueExpired"));
        },Terminal);
    return true;
}
void VaRest::Http::FOfflineQueue::ReplayFinished(FString QueueId,FVaRestQueuedResult Result,bool bKeep)
{
    LLM_SCOPE_BYTAG(VaRest);
    if (bKeep)
    {
        bReplayActive=false;
        const bool bWasProbe=bProbeReplay; bProbeReplay=false;
        if (const auto Reachability=Connectivity.Pin())
        {
            if (bWasProbe) Reachability->ProbeFinished();
        }
        // Logical Connection is not physical evidence. Pause this queue, not shared reachability.
        ReplayNotBefore=QueueClock->Now()+10.0;
        QueueClock->ClearTimer(ReplayRetryTimer);
        const TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe> Weak(AsShared());
        if (!bStopped && !FClientState::IsShuttingDown())
            ReplayRetryTimer=QueueClock->SetTimer(10.0,[Weak]
            { if (const auto Self=Weak.Pin()) Self->ScheduleReplay(); });
        return;
    }
    RetireAndNotify(MoveTemp(QueueId),MoveTemp(Result));
}
void VaRest::Http::FOfflineQueue::RetireAndNotify(FString QueueId,FVaRestQueuedResult Result)
{
    LLM_SCOPE_BYTAG(VaRest);
    if (Items.IsEmpty() || Items[0].QueueId!=QueueId) { bStorageFailed=true; bReplayActive=false; return; }
    FQueueStore::Retire(Directory,Items[0].Sequence,QueueId,
        [Self=AsShared(),QueueId=MoveTemp(QueueId),Result=MoveTemp(Result)](bool bOk) mutable
        {
            LLM_SCOPE_BYTAG(VaRest);
            if (bOk && !Self->Items.IsEmpty() && Self->Items[0].QueueId==QueueId)
            {
                Self->CommittedBodyBytes-=Self->Items[0].Body.Bytes.Num();
                Self->CommittedMetadataBytes-=QueueMetadataCharge(Self->Items[0]); Self->Items.RemoveAt(0);
            }
            else Self->bStorageFailed=true;
            Self->bReplayActive=false;
            const bool bWasProbe=Self->bProbeReplay; Self->bProbeReplay=false;
            if (bWasProbe)
                if (const auto Reachability=Self->Connectivity.Pin()) Reachability->ProbeFinished();
            if (bOk) { Self->BroadcastLater(MoveTemp(Result)); Self->ScheduleReplay(); }
        });
}
void VaRest::Http::FOfflineQueue::BroadcastLater(FVaRestQueuedResult Result)
{
    LLM_SCOPE_BYTAG(VaRest);
    // Called only from the asynchronous retirement handback: already later than Send/network completion.
    if (bStopped || FClientState::IsShuttingDown()) return;
    const auto Keep=AsShared();
    TArray<uint64> Ids; Members.GetKeys(Ids); Ids.Sort();
    TArray<TSharedPtr<FClientState,ESPMode::ThreadSafe>> Owners;
    for (uint64 Id : Ids)
        if (const auto Member=Members.FindChecked(Id).Pin())
            if (Member->Queue.Get()==this) Owners.Add(Member);
    for (const auto& Member : Owners)
    { VAREST_LLM_SCOPE_SENDER(Member->EventSenderTags); Member->QueuedResult.Broadcast(Result); }
}

bool VaRest::Http::FOfflineQueue::Flush(TUniqueFunction<void(bool)>&& Done)
{
    LLM_SCOPE_BYTAG(VaRest);
    check(IsInGameThread());
    if (bStopped || FClientState::IsShuttingDown() || FlushWaiters.Num()>=4096) return false;
    auto Waiter=MakeShared<FFlushWaiter,ESPMode::ThreadSafe>();
    Waiter->Through=LastAdmission;
    Waiter->Done=MoveTemp(Done);
    // Reserve the delivery slot before retaining a waiter. It remains charged until delivery/discard.
    if (!PostPersistencePoll([Self=AsShared(),Waiter]{ return Self->PollFlush(Waiter); })) return false;
    FlushWaiters.Add(Waiter);
    return true;
}
void VaRest::Http::FOfflineQueue::FinishAdmission(uint64 Ticket)
{
    check(IsInGameThread()); ActiveAdmissions.Remove(Ticket);
    // PollFlush owns readiness and delivery. Never invoke a flush callback on an API stack.
}
bool VaRest::Http::FOfflineQueue::PollFlush(const TSharedRef<FFlushWaiter,ESPMode::ThreadSafe>& Waiter)
{
    if (bStopped || FClientState::IsShuttingDown()) { FlushWaiters.Remove(Waiter); return true; }
    if (Waiter->Result.IsSet())
    {
        const bool bOk=Waiter->Result.GetValue();
        auto Callback=MoveTemp(Waiter->Done);
        FlushWaiters.Remove(Waiter);
        if (Callback) Callback(bOk);
        return true;
    }
    LLM_SCOPE_BYTAG(VaRest);
    if (!bLoaded || Waiter->bBarrierStarted) return false;
    for (uint64 Ticket : ActiveAdmissions) if (Ticket<=Waiter->Through) return false;
    Waiter->bBarrierStarted=true;
    if (!LaunchPersistence([]{ FIoResult Result; Result.bOk=true; return Result; },
        [Self=AsShared(),Waiter](FIoResult&& Result)
        { Waiter->Result=Result.bOk && !Self->bStorageFailed && !Self->bFlushFailed; }))
        Waiter->Result=false;
    return false; // both refusal and worker success are delivered by a subsequent owned poll
}
