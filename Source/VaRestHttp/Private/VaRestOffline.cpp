// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestOffline.h"
#include "VaRestOfflineInternal.h"
#include "VaRestQueueStore.h"
#include "VaRestQueueReplay.h"
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
#include "VaRestManagedCallbacks.h"
#include "VaRestJson.h"
#include "VaRestUrl.h"
#include "Misc/Paths.h"
#include "Misc/Crc.h"

namespace VaRest::Http::OfflinePrivate
{
	struct FRegistryEntry
	{
		uint64 Epoch=0;
		TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe> Queue;
	};
    struct FRootKeys : TDefaultMapHashableKeyFuncs<FString,FRegistryEntry,false>
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
    using FRegistry=TMap<FString,FRegistryEntry,FDefaultSetAllocator,FRootKeys>;
    FRegistry& Registry()
    { static auto* Values=new FRegistry(); return *Values; }
	uint64 AllocateRegistryEpoch()
	{
		static uint64 Next=1;
		const uint64 Result=Next;
		Next=Next==MAX_uint64 ? 0 : (Next==0 ? 0 : Next+1);
		return Result;
	}
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
{ return RequestIdentityScope(*Ex.Config,Ex.State->GetSessionData(),Ex.AuthMode,Ex.RequestHeaders,OutScope,&Ex.State->GetManagedLifetime()); }
bool VaRest::Http::QueueIdentityStillCurrent(const FExchangeState& Ex)
{
    if (Ex.QueueIdentityScope.IsEmpty()) return false;
    const auto Current=Ex.State->GetConfig();
    if (Ex.AuthMode!=EVaRestAuthMode::None && Current->Auth!=Ex.Config->Auth) return false;
    FString Scope,Origin;
    return CanonicalOrigin(Ex.Url,Origin) && Origin==Ex.QueueAnchorOrigin
        && SameOrigin(Current->BaseUrl,Ex.Url)
        && RequestIdentityScope(*Current,Ex.State->GetSessionData(),Ex.AuthMode,Ex.QueueScopeHeaders,Scope,&Ex.State->GetManagedLifetime())
        && Scope==Ex.QueueIdentityScope;
}
bool VaRest::Http::SameQueueRegistrationTarget(const FClientConfig& A,const FClientConfig& B)
{
	return A.BaseUrl.Equals(B.BaseUrl,ESearchCase::CaseSensitive) && A.QueueName.Equals(B.QueueName,ESearchCase::CaseSensitive) &&
		A.QueueRoot.Equals(B.QueueRoot,ESearchCase::CaseSensitive) && A.StorageConfigError==B.StorageConfigError &&
		A.Connectivity==B.Connectivity && A.Auth==B.Auth && OfflinePrivate::SamePolicy(A.OfflinePolicy,B.OfflinePolicy);
}
bool VaRest::Http::FOfflineQueue::ValidateTarget(const FClientConfig& Config,FString& OutConfigError)
{
	OutConfigError.Reset();
	FString Canonical;
	if (!Config.StorageConfigError.IsEmpty() || !Config.Connectivity.IsValid() || !Config.Clock.IsValid() ||
		!CanonicalOrigin(Config.BaseUrl,Canonical))
	{ OutConfigError=TEXT("QueueConfig");return false; }
	if (Config.QueueName.IsEmpty()) return true;
	if (Config.QueueRoot.Len()>4096 || !OfflinePrivate::PlainName(Config.QueueName) ||
		!OfflinePrivate::ValidPolicy(Config.OfflinePolicy) || Config.QueueName.Len()>1024 || !IsValidHeaderValue(Config.QueueName))
	{ OutConfigError=TEXT("QueueConfig");return false; }
	const FString Path=QueueDirectory(Config.QueueRoot,Config.QueueName);
	if (Path.Len()>4096 || Path.GetAllocatedSize()>8192*sizeof(TCHAR))
	{ OutConfigError=TEXT("QueueConfig");return false; }
	return true;
}
TSharedPtr<VaRest::Http::FOfflineQueue,ESPMode::ThreadSafe> VaRest::Http::FOfflineQueue::Join(
    const TSharedRef<FClientState,ESPMode::ThreadSafe>& Client,FString& OutConfigError,
	TSharedPtr<FQueueBinding,ESPMode::ThreadSafe>* OutBinding)
{
    LLM_SCOPE_BYTAG(VaRest);
    check(IsInGameThread()); OutConfigError.Reset();
	if (OutBinding) OutBinding->Reset();
	const auto& Native=Client->GetManagedLifetime();
	if (Native.IsValid() && (Native.Control->IsStopped() || Native.Gate->IsClosed()))
	{ OutConfigError=TEXT("QueueDetached");return nullptr; }
    const auto Config=Client->GetConfig();
	if (Native.IsValid() && !Config->bQueueActivationRequested)
	{ OutConfigError=Config->QueueName.IsEmpty() ? TEXT("QueueAccountRequired") : TEXT("QueueNotActivated");return nullptr; }
    if (Config->QueueName.IsEmpty()) return nullptr;
    FString Canonical;
    if (FClientState::IsShuttingDown() || !PersistenceAccepting()) { OutConfigError=TEXT("QueueConfig"); return nullptr; }
    if (!ValidateTarget(*Config,OutConfigError)) return nullptr;
    CanonicalOrigin(Config->BaseUrl,Canonical); // validated above, before all provider/clock callouts
    const FString Path=QueueDirectory(Config->QueueRoot,Config->QueueName);
	FNativeQueueBindingReservation Reservation;
	if (Native.IsValid())
	{
		FName Error;
		if (!Native.Control->ClientSlotsOnGT().ReserveQueueBinding(Native,Reservation,Error))
		{ OutConfigError=Error.ToString();return nullptr; }
	}
    const FString Category=Config->Auth.IsValid() ? Config->Auth->GetAuthName().ToString().ToLower() : FString(TEXT("None"));
	if (Native.IsValid() && (Native.Control->IsStopped() || Native.Gate->IsClosed()))
	{ OutConfigError=TEXT("QueueDetached");return nullptr; }
    const FName Contract=Config->Auth.IsValid() ? Config->Auth->GetIdentityContractName() : NAME_None;
	if (Native.IsValid() && (Native.Control->IsStopped() || Native.Gate->IsClosed()))
	{ OutConfigError=TEXT("QueueDetached");return nullptr; }
    auto& Registry=OfflinePrivate::Registry();
    for (auto It=Registry.CreateIterator();It;++It) if (!It.Value().Queue.IsValid()) It.RemoveCurrent();
    auto Queue=Registry.FindRef(Path).Queue.Pin();
	if (Queue && Queue->CoordinatorPhase==ECoordinatorPhase::Draining)
	{
		Queue->TryFenceNamespaceOnGT(); // exact old pointer/epoch only; a failed/refused Flush is not claimed durable
		Queue=Registry.FindRef(Path).Queue.Pin();
	}
	if (Native.IsValid() && (Native.Control->IsStopped() || Native.Gate->IsClosed()))
	{ OutConfigError=TEXT("QueueDetached");return nullptr; }
	if (!SameQueueRegistrationTarget(*Config,*Client->GetConfig()))
	{ OutConfigError=TEXT("QueueBusy");return nullptr; }
	bool bCreated=false;
    if (Queue.IsValid())
    {
		if (Queue->CoordinatorPhase!=ECoordinatorPhase::Live)
		{ OutConfigError=TEXT("QueueBusy");return nullptr; }
        if (Queue->bStopped || Queue->Origin!=Canonical || Queue->Connectivity.Pin()!=Config->Connectivity
            || !OfflinePrivate::SamePolicy(Queue->Policy,Config->OfflinePolicy) || Queue->AuthCategory!=Category
            || Queue->IdentityContract!=Contract
            || (Contract.IsNone() && Queue->IdentityProvider.Pin()!=Config->Auth))
        { OutConfigError=Native.IsValid() ? TEXT("QueueBusy") : TEXT("QueueConfig"); return nullptr; }
    }
    else
    {
		const uint64 Epoch=OfflinePrivate::AllocateRegistryEpoch();
		if (Epoch==0) { OutConfigError=TEXT("QueueBusy");return nullptr; }
        Queue=MakeShared<FOfflineQueue,ESPMode::ThreadSafe>();
        Queue->Directory=Path; Queue->Origin=Canonical; Queue->Policy=Config->OfflinePolicy;
        Queue->Connectivity=Config->Connectivity; Queue->AuthCategory=Category;
        Queue->IdentityContract=Contract; Queue->IdentityProvider=Config->Auth;
		Queue->RegistryEpoch=Epoch;Queue->QueueClock=Config->Clock;
		Registry.Add(Path,OfflinePrivate::FRegistryEntry{Epoch,Queue}); // Initializing bars a reentrant second writer
		bCreated=true;
		Queue->MonotonicOrigin=Config->Clock->Now();Queue->UtcOrigin=FDateTime::UtcNow();
    }
	const auto Refuse=[&](const TCHAR* Error)->TSharedPtr<FOfflineQueue,ESPMode::ThreadSafe>
	{
		if (bCreated)
		{
			Queue->CoordinatorPhase=ECoordinatorPhase::Fenced;Queue->bStopped=true; // no load/IO/registration was published
			const auto* Entry=Registry.Find(Path);
			if (Entry && Entry->Epoch==Queue->RegistryEpoch && Entry->Queue.Pin()==Queue) Registry.Remove(Path);
		}
		OutConfigError=Error;return nullptr;
	};
	if (!SameQueueRegistrationTarget(*Config,*Client->GetConfig()) ||
		(Native.IsValid() && (Native.Control->IsStopped() || Native.Gate->IsClosed()))) return Refuse(TEXT("QueueBusy"));
	if (Queue->NextMemberEpoch==0) { OutConfigError=TEXT("QueueConfig"); return nullptr; }
	const FQueueMemberKey Key{Client->GetClientId(),Queue->NextMemberEpoch};
	Queue->NextMemberEpoch=Queue->NextMemberEpoch==MAX_uint64 ? 0 : Queue->NextMemberEpoch+1;
	const auto Binding=MakeShared<FQueueBinding,ESPMode::ThreadSafe>(Key,Queue->CaptureGate);
	if (Native.IsValid() && !Reservation.Commit(Queue.ToSharedRef(),Binding)) return Refuse(TEXT("QueueDetached"));
	Queue->Members.Add(Key,FMember{Client,Binding});
	Queue->CoordinatorPhase=ECoordinatorPhase::Live;
	if (OutBinding) *OutBinding=Binding;
	if (bCreated)
	{
		Queue->Load();
		const TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe> Weak(Queue);
		Queue->ConnectivityChanged=Config->Connectivity->OnChanged().AddLambda([Weak](bool bOnline)
		{ if (bOnline) if (const auto Pinned=Weak.Pin()) Pinned->ScheduleReplay(); });
	}
    Queue->ScheduleReplay();
    return Queue;
}
void VaRest::Http::FOfflineQueue::Leave(uint64 ClientId)
{
	check(IsInGameThread());
	// Compatibility for private legacy callers. Client lifetime/reconfiguration uses the exact binding overload.
	for (auto It=Members.CreateIterator();It;++It)
		if (It.Key().ClientId==ClientId)
		{
			It.Value().Binding->CloseCaptureAdmission();
			It.RemoveCurrent();
		}
	BeginLastMemberDrain();
}
void VaRest::Http::FOfflineQueue::Leave(const TSharedPtr<FQueueBinding,ESPMode::ThreadSafe>& Binding,
	TSharedPtr<FNativeLifetimeControl,ESPMode::ThreadSafe> Control)
{
	check(IsInGameThread());
	if (!Binding) return;
	const auto Keep=AsShared();
	Binding->CloseCaptureAdmission();
	const FMember* Found=Members.Find(Binding->GetKey());
	if (Found && Found->Binding==Binding) Members.Remove(Binding->GetKey());
	BeginLastMemberDrain();
	if (!Binding->AllowsEnqueue())
	{
		const auto Waiters=FlushWaiters; // callback destruction can reenter and mutate the live array
		for (const auto& Waiter:Waiters)
			if (Waiter->OriginBinding==Binding)
			{
				auto Dropped=MoveTemp(Waiter->Done);
				Dropped.Reset(); // actual user payload removed on GT; the owned poll/IO witness remains independent
			}
	}
	if (Control && Control->IsStopped()) return; // no normal clock/replay/cancel call after a reentrant Stop
	const auto Replay=ManagedReplay;
	if (Replay && !Binding->AllowsEnqueue() && Replay->MemberKey()==Binding->GetKey())
		Replay->DetachOwnerOnGT();
}
int32 VaRest::Http::FOfflineQueue::Count() const { check(IsInGameThread()); return Items.Num()+ReservedCount+AfterLoad.Num(); }
void VaRest::Http::FOfflineQueue::BeginLastMemberDrain()
{
	check(IsInGameThread());
	for (auto It=Members.CreateIterator();It;++It)
		if (!It.Value().Client.IsValid())
		{ It.Value().Binding->CloseCaptureAdmission();It.RemoveCurrent(); }
	if (!Members.IsEmpty() || CoordinatorPhase!=ECoordinatorPhase::Live) return;
	const auto Keep=AsShared();
	CoordinatorPhase=ECoordinatorPhase::Draining;
	CaptureGate->Close(); // one atomic order includes senders paused between aggregate and binding acquisition
	if (!bStopped && QueueClock) QueueClock->ClearTimer(ReplayRetryTimer);
}
bool VaRest::Http::FOfflineQueue::MemberDrainReadyOnGT(
	const TSharedPtr<FQueueBinding,ESPMode::ThreadSafe>& Binding,uint64 Through) const
{
	check(IsInGameThread());
	if (!Binding || !Binding->CaptureAdmissionIsClosed() || Binding->NumCaptures()!=0 || Through>LastAdmission || !bLoaded) return false;
	for (const auto& Ticket:ActiveAdmissions)
		if (Ticket.Value==Binding->GetKey()) return false; // includes permitted old captures admitted after the selected watermark
	for (const auto& Waiter:FlushWaiters)
		if (Waiter->OriginBinding==Binding) return false; // own accepted public Flush, including its actual GT callback stack
	if (bReplayActive && ReplayOwner==Binding->GetKey()) return false;
	return true; // another compatible member's later requests/IO do not enlarge this member's wait
}
bool VaRest::Http::FOfflineQueue::FlushOwned(const TSharedPtr<FQueueBinding,ESPMode::ThreadSafe>& Binding,
	uint64 Through,FNativeQueueFenceToken&& Token)
{
	check(IsInGameThread());
	if (!Token.IsValid() || bStopped || CoordinatorPhase==ECoordinatorPhase::Fenced ||
		!MemberDrainReadyOnGT(Binding,Through) || !Token.AttachNamespaceCounter(OwnedFlushes)) return false;
	FIoDone Done=[Held=MoveTemp(Token)](FIoResult&& Result) mutable
	{ Held.SettleOnce(Result.bOk ? EQueueFenceResult::Flushed : EQueueFenceResult::Failed); };
	// SAME serial persistence pipe/barrier as public Flush. No new waiter/poll admission or callback-owned queue/client.
	return LaunchPersistence([]{ FIoResult Result;Result.bOk=true;return Result; },MoveTemp(Done),0,true);
}
bool VaRest::Http::FOfflineQueue::TryFenceNamespaceOnGT()
{
	check(IsInGameThread());
	if (CoordinatorPhase==ECoordinatorPhase::Fenced) return true;
	if (CoordinatorPhase==ECoordinatorPhase::Initializing) return false;
	const auto Keep=AsShared();
	BeginLastMemberDrain();
	if (CoordinatorPhase==ECoordinatorPhase::Live) return true; // compatible B keeps its coordinator, while A's row completes
	if (CoordinatorPhase!=ECoordinatorPhase::Draining || !Members.IsEmpty() || !bLoaded ||
		CaptureGate->Count()!=0 || !AfterLoad.IsEmpty() || !ActiveAdmissions.IsEmpty() || ReservedCount!=0 ||
		bReplayActive || ManagedReplay || OwnedDrains!=0 || !FlushWaiters.IsEmpty() || OwnedFlushes->load(std::memory_order_acquire)!=0) return false;
	CoordinatorPhase=ECoordinatorPhase::Fenced; // revoke all future scheduling/IO BEFORE removing only our exact registry entry
	bStopped=true; // inert old shell; the timer was revoked when entering Draining, and is never touched after fencing
	auto& Registry=OfflinePrivate::Registry();
	const auto* Entry=Registry.Find(Directory);
	if (Entry && Entry->Epoch==RegistryEpoch && Entry->Queue.Pin().Get()==this) Registry.Remove(Directory);
	if (const auto Reachability=Connectivity.Pin()) Reachability->OnChanged().Remove(ConnectivityChanged);
	ConnectivityChanged.Reset();
	Items.Reset();CommittedBodyBytes=0;CommittedMetadataBytes=0;
	// No persistent files are deleted. Failed/refused Flush permits only quiescent recovery, not a durable-success claim.
	return true;
}
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
    if (bStopped || CoordinatorPhase==ECoordinatorPhase::Fenced || BodyBytes < 0 || BodyBytes > Policy.MaxItemBytes || BodyBytes > Policy.MaxBodyBytes)
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
    if (!bStopped && QueueClock.IsValid()) QueueClock->ClearTimer(ReplayRetryTimer);
}
void VaRest::Http::FOfflineQueue::StopForShutdown()
{
    check(IsInGameThread()); bStorageFailed=true; bStopped=true;
	const auto Keep=AsShared();
	if (bStopCleanupActive) return;
	TGuardValue<bool> Cleaning(bStopCleanupActive,true);
	const auto Replay=ManagedReplay;
	if (Replay && Replay->bManagedSettled && Replay->Phase!=FQueueReplayOperation::EPhase::RetireIo)
		ReleaseManagedReplay(Replay.ToSharedRef(),false);
    AfterLoad.Reset(); WaitingBodyBytes=0;
	const auto Waiters=FlushWaiters;
	for (const auto& Waiter:Waiters)
	{
		auto Dropped=MoveTemp(Waiter->Done);
		Dropped.Reset(); // suppression destroys the actual callback on GT; native waiter/poll lifetime is separate
	}
	FlushWaiters.RemoveAll([](const auto& Waiter){ return !Waiter->bDelivering; });
    if (QueueClock.IsValid()) QueueClock->ClearTimer(ReplayRetryTimer);
}
void VaRest::Http::FOfflineQueue::StopAll()
{
    check(IsInGameThread());
    TArray<TSharedPtr<FOfflineQueue,ESPMode::ThreadSafe>> Alive;
    for (const auto& Pair : OfflinePrivate::Registry()) if (const auto Queue=Pair.Value.Queue.Pin()) Alive.Add(Queue);
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
        if (Internals(*Exchange).Managed.IsValid())
        {
            if (IsManagedStopped(Exchange)) { ResolveCleanup(false); return; }
            const auto Holder=AbortOwner.Pin();
            check(Holder && !*Holder && AbortDone);
            *Holder=AsShared(); // actual job owner is in the reserved GT slot, never in the IO adapter
            CommitDone.Reset();
            FQueueStore::Abort(Queue->GetDirectory(),Item.Sequence,Item.QueueId,MoveTemp(AbortDone));
            return;
        }
        // Keep the job, admission ticket and reservation until the actual owned cleanup result.
        FQueueStore::Abort(Queue->GetDirectory(),Item.Sequence,Item.QueueId,
            [Self=AsShared()](bool bClean){ Self->ResolveCleanup(bClean); });
    }
    bool PrepareCallbacks();
    void Prepared(FQueuePrepareResult PrepareResult);
    void Committed(FQueueCommitResult Result,FVaRestOutcomeCallback Done);
    FVaRestExchangeRef Exchange;
    TSharedRef<FPersistenceWait,ESPMode::ThreadSafe> Wait;
    FQueueItem Item;
    TSharedRef<FOfflineQueue,ESPMode::ThreadSafe> Queue;
    int64 ReservedBytes,ReservedMetadata;
    bool bPrepared=false,bReserved=true;
    TSharedPtr<FQueueAdmissionToken,ESPMode::ThreadSafe> Admission;
    // Reserve all possible IO handbacks BEFORE Prepare. A later detach/child closure never admits another tail.
    TUniqueFunction<void(FQueuePrepareResult)> PrepareDone;
    TUniqueFunction<void(FQueueCommitResult)> CommitDone;
    TUniqueFunction<void(bool)> AbortDone;
    TWeakPtr<TSharedPtr<FQueueAdmissionJob,ESPMode::ThreadSafe>,ESPMode::ThreadSafe> CommitOwner,AbortOwner;
    FVaRestOutcomeCallback Accepted;
};

bool FQueueAdmissionJob::PrepareCallbacks()
{
    check(IsInGameThread());
    if (!Internals(*Exchange).Managed.IsValid())
    {
        PrepareDone=[Self=AsShared()](FQueuePrepareResult Result) { Self->Prepared(Result); };
        return true;
    }
    const auto CommitHolder=MakeShared<TSharedPtr<FQueueAdmissionJob,ESPMode::ThreadSafe>,ESPMode::ThreadSafe>();
    const auto AbortHolder=MakeShared<TSharedPtr<FQueueAdmissionJob,ESPMode::ThreadSafe>,ESPMode::ThreadSafe>();
    CommitOwner=CommitHolder; AbortOwner=AbortHolder;
    // Empty until selected. The job owns only native adapters and WEAK holder refs, so an uncalled Prepare drop
    // releases the job on GT and then drops the unused native adapters; no anchor/job/adapter self-cycle remains.
    CommitDone=[CommitHolder](FQueueCommitResult Result) {
        const auto Self=MoveTemp(*CommitHolder);
        if (Self) Self->Committed(Result,MoveTemp(Self->Accepted));
    };
    AbortDone=[AbortHolder](bool bClean) {
        const auto Self=MoveTemp(*AbortHolder);
        if (Self) Self->ResolveCleanup(bClean);
    };
    if (!PrepareManagedQueueCommit(Exchange,CommitDone) || !PrepareManagedQueueCleanup(Exchange,AbortDone)) return false;
    PrepareDone=[Self=AsShared()](FQueuePrepareResult Result) { Self->Prepared(Result); };
    return PrepareManagedQueuePrepare(Exchange,PrepareDone);
}

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
	if (Ex.QueueCapture.IsHeld() && !Ex.QueueCapture.GetBinding()->AllowsEnqueue())
	{ OnOutcome(LocalFailure(TEXT("QueueDetached"))); return; }
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
    FIoDone Checked=[Exchange,Next,Wait](FIoResult&& Result) mutable
    {
        LLM_SCOPE_BYTAG(VaRest);
        if (!Wait->IsPending()) return;
        if (!Result.bOk) { Wait->Finish(LocalFailure(TEXT("PersistenceCredentials"))); return; }
        FVaRestOutcomeCallback Resume=Wait->Resume();
        if (Resume) Next.Proceed(Exchange,MoveTemp(Resume));
    };
    if (!PrepareManagedPersistenceResult(Exchange,Checked))
    {
        if (!IsManagedStopped(Exchange)) Wait->Finish(MakeManagedCapacityOutcome(TEXT("LogicalCapacity")));
        return;
    }
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
    },MoveTemp(Checked),Reservation);
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
	if (CoordinatorPhase==ECoordinatorPhase::Fenced) { Done(LocalFailure(TEXT("QueueDetached")));return; }
    auto Wait=FPersistenceWait::Begin(Exchange,MoveTemp(Done));
    if (!Wait->IsPending()) return;
	const auto& Capture=Internals(*Exchange).QueueCapture;
	if (!Capture.IsHeld() || !Capture.GetBinding()->AllowsEnqueue())
	{ Wait->Finish(LocalFailure(TEXT("QueueDetached"))); return; }
    if (LastAdmission==MAX_uint64) { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
    const uint64 Ticket=++LastAdmission; ActiveAdmissions.Add(Ticket,Capture.GetBinding()->GetKey());
    const auto Admission=MakeShared<FQueueAdmissionToken,ESPMode::ThreadSafe>(AsShared(),Ticket);
    TUniqueFunction<void()> Start=[Self=AsShared(),Exchange,Wait,Admission]
    {
        LLM_SCOPE_BYTAG(VaRest);
        if (!Wait->CanHandoff()) return;
        auto& Ex=Internals(*Exchange);
        FString Scope,Error;
        if (Self->bStopped || Self->bStorageFailed || Self->NextSequence==0)
        { Wait->Finish(LocalFailure(TEXT("QueueWriteFailed"))); return; }
		if (!Ex.QueueCapture.IsHeld() || !Ex.QueueCapture.GetBinding()->AllowsEnqueue())
		{ Wait->Finish(LocalFailure(TEXT("QueueDetached"))); return; }
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
        Item.Endpoint=Ex.Endpoint.IsNone() ? FString() : Ex.Endpoint.ToString();
        if (Item.Tag.Len()>256 || Item.Endpoint.Len()>256) { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
        const auto Measured=MeasureQueueAdmissionMetadata(Item,Self->Origin,Self->Policy);
        if (!Measured.IsSet()) { Wait->Finish(LocalFailure(TEXT("QueueWriteFailed"))); return; }
        const int64 MetadataBytes=Measured.GetValue(); Item.MetadataCharge=MetadataBytes;
        if (!Self->Reserve(BodyBytes,MetadataBytes,Error)) { Wait->Finish(LocalFailure(FName(*Error))); return; }
        // No payload copy occurs before the count/body/metadata reservation succeeds.
        TArray<uint8> OwnedBytes=CopyQueueAdmissionBytes(Ex.Body.Bytes);
        Item.Body=FVaRestRequestBody::FromBytes(MoveTemp(OwnedBytes),Ex.Body.Kind,Item.Body.ContentType);
        const auto Job=MakeShared<FQueueAdmissionJob,ESPMode::ThreadSafe>(Exchange,Wait,MoveTemp(Item),Self,BodyBytes,MetadataBytes);
        if (!Job->PrepareCallbacks())
        {
            Job->ResolveCleanup(true); // no IO was submitted
            if (!IsManagedStopped(Exchange)) Wait->Finish(MakeManagedCapacityOutcome(TEXT("LogicalCapacity")));
            return;
        }
        FQueueItem WorkerItem=Job->Item; // bounded second body copy, after the same successful reservation
#if WITH_DEV_AUTOMATION_TESTS
        Testing::Counters().QueueAdmissionBytesCopied.fetch_add(BodyBytes);
#endif
        Job->Admission=Admission;
        FQueueStore::Prepare(Self->Directory,Self->Origin,Self->Policy,MoveTemp(WorkerItem),MoveTemp(Guard),
            MoveTemp(Job->PrepareDone));
    };
    if (!PrepareManagedGTCallback(Exchange,Start))
    {
        if (!IsManagedStopped(Exchange)) Wait->Finish(MakeManagedCapacityOutcome(TEXT("LogicalCapacity")));
        return;
    }
    if (bLoaded) Start();
    else if (AfterLoad.Num()>=Policy.MaxQueued) Wait->Finish(LocalFailure(TEXT("QueueFull")));
    else
    {
        const int64 WaitingBytes=Internals(*Exchange).Body.Bytes.Num();
        if (WaitingBytes>Policy.MaxItemBytes || WaitingBodyBytes>Policy.MaxBodyBytes-WaitingBytes)
        { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
        WaitingBodyBytes+=WaitingBytes;
        AfterLoad.Add([Self=AsShared(),Start=MoveTemp(Start),WaitingBytes]() mutable
        { Self->WaitingBodyBytes-=WaitingBytes; Start(); });
    }
}
void VaRest::Http::FOfflineQueue::AcceptCommitted(FQueueItem Item)
{
    CommittedBodyBytes+=Item.Body.Bytes.Num(); CommittedMetadataBytes+=QueueMetadataCharge(Item); Items.Add(MoveTemp(Item));
    Items.Sort([](const FQueueItem& Left,const FQueueItem& Right){ return Left.Sequence<Right.Sequence; });
}

void VaRest::Http::FQueueAdmissionJob::Prepared(FQueuePrepareResult PrepareResult)
{
    LLM_SCOPE_BYTAG(VaRest);
    auto& Ex=Internals(*Exchange);
    if (!PrepareResult.bPrepared)
    {
        Queue->bFlushFailed=true;
        ResolveCleanup(PrepareResult.bCleanupOk);
        if (Wait->IsPending()) Wait->Finish(LocalFailure(TEXT("QueueWriteFailed")));
        return;
    }
    bPrepared=true;
    const auto Current=Ex.State->GetConfig();
    const bool bValidIdentity=QueueIdentityStillCurrent(Ex) && Item.Scope==Ex.QueueIdentityScope;
    if (IsManagedStopped(Exchange)) { ResolveCleanup(false);return; }
    const bool bValidOrigin=SameOrigin(Current->BaseUrl,Item.Url);
    const bool bDeadline=Ex.DeadlineAt>0 && Ex.Config->Clock->Now()>=Ex.DeadlineAt-DeadlineTimerSlack;
    if (IsManagedStopped(Exchange)) { ResolveCleanup(false);return; }
	const bool bDetached=!Ex.QueueCapture.IsHeld() || !Ex.QueueCapture.GetBinding()->AllowsEnqueue();
    if (!Wait->IsPending() || Ex.IsCancelRequested() || IsOwnerGone(Ex) || bDeadline
        || bDetached || !bValidIdentity || !bValidOrigin || FClientState::IsShuttingDown())
    {
        EQueueAdmission Expected=EQueueAdmission::Open;
        Ex.QueueAdmission.compare_exchange_strong(Expected,EQueueAdmission::Aborted,std::memory_order_acq_rel);
        AbortPrepared();
        if (Wait->IsPending())
        {
            if (bDeadline) Wait->Finish(MakeDeadlineOutcome());
			else if (bDetached) Wait->Finish(LocalFailure(TEXT("QueueDetached")));
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
    FVaRestOutcomeCallback Selected=Wait->TakeCompletion(); // already past the shared commit CAS; no later deadline override
    if (Internals(*Exchange).Managed.IsValid())
    {
        if (IsManagedStopped(Exchange)) { ResolveCleanup(false);return; }
        const auto Holder=CommitOwner.Pin();
        check(Holder && !*Holder && CommitDone);
        *Holder=AsShared();
        Accepted=MoveTemp(Selected);
        AbortDone.Reset();
        FQueueStore::Commit(Queue->GetDirectory(),Item.Sequence,Item.QueueId,MoveTemp(CommitDone));
        return;
    }
    FQueueStore::Commit(Queue->GetDirectory(),Item.Sequence,Item.QueueId,
        [Self=AsShared(),Done=MoveTemp(Selected)](FQueueCommitResult Result) mutable { Self->Committed(Result,MoveTemp(Done)); });
}
void VaRest::Http::FQueueAdmissionJob::Committed(FQueueCommitResult Result,FVaRestOutcomeCallback Done)
{
    LLM_SCOPE_BYTAG(VaRest);
    auto& Ex=Internals(*Exchange);
    ResolveCleanup(Result.bCleanupOk);
    if (!Result.bCommitted)
    {
        Queue->bFlushFailed=true;
        if (Done && !IsManagedStopped(Exchange)) Done(LocalFailure(TEXT("QueueWriteFailed")));
        return;
    }
    Queue->AcceptCommitted(Item); // actual accepted disk result remains truthful across runtime membership loss
    Ex.bQueueCommitted=true;
    if (!Ex.bFinished && !Ex.bCompletionQueued && Ex.Phase==EVaRestExchangePhase::InChain && !IsManagedStopped(Exchange))
    {
        FVaRestTraceNote Note; Note.Kind=EVaRestTraceNoteKind::Queued;
        Note.QueueId=Item.QueueId; Note.Reason=TEXT("Offline");
        TracePoints::Note(Ex,MoveTemp(Note));
    }
    FVaRestAttemptOutcome Outcome; Outcome.bQueued=true; Outcome.QueueId=Item.QueueId;
    if (Done && !IsManagedStopped(Exchange)) Done(MoveTemp(Outcome));
    if (!IsManagedStopped(Exchange)) Queue->ScheduleReplay();
}

TSharedPtr<VaRest::Http::FClientState,ESPMode::ThreadSafe> VaRest::Http::FOfflineQueue::SelectOwner() const
{
	TArray<FQueueMemberKey> Keys; Members.GetKeys(Keys);
	Keys.Sort([](const FQueueMemberKey& A,const FQueueMemberKey& B)
	{ return A.ClientId<B.ClientId || (A.ClientId==B.ClientId && A.Epoch<B.Epoch); });
	for (const auto& Key : Keys)
	{
		const auto& Member=Members.FindChecked(Key);
		if (const auto Candidate=Member.Client.Pin())
			if (Candidate->Queue.Get()==this && Candidate->QueueBinding==Member.Binding && Member.Binding->AllowsEnqueue()) return Candidate;
	}
    return nullptr;
}
void VaRest::Http::FOfflineQueue::ScheduleReplay()
{
    LLM_SCOPE_BYTAG(VaRest);
    if (CoordinatorPhase!=ECoordinatorPhase::Live || bStopped || bReplayScheduled || !bLoaded || bStorageFailed || Items.IsEmpty() || FClientState::IsShuttingDown()) return;
    bReplayScheduled=true;
    const TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe> Weak(AsShared());
    if (!PostPersistenceContinuation([Weak]
    {
        if (const auto Self=Weak.Pin()) { Self->bReplayScheduled=false; Self->TryReplay(false); }
    })) { bReplayScheduled=false; bStorageFailed=true; }
}
bool VaRest::Http::FOfflineQueue::TryReplay(bool bProbe,FVaRestProbeToken ProbeToken)
{
    LLM_SCOPE_BYTAG(VaRest);
    check(IsInGameThread());
    if (CoordinatorPhase!=ECoordinatorPhase::Live || bStopped || !bLoaded || bStorageFailed || bReplayActive || Items.IsEmpty() || FClientState::IsShuttingDown()
        || QueueClock->Now()<ReplayNotBefore) return false;
    const auto Reachability=Connectivity.Pin();
    const auto Owner=SelectOwner();
    if (!Reachability.IsValid() || !Owner.IsValid() || (!bProbe && !Reachability->IsOnline())) return false;
    bReplayActive=true; bProbeReplay=bProbe;
	ReplayOwner=Owner->QueueBinding ? Owner->QueueBinding->GetKey() : FQueueMemberKey{};
	ReplayProbeToken=ProbeToken;
    const FQueueItem Item=Items[0];
    FName Terminal;
    const auto Current=Owner->GetConfig();
    FString Scope;
    const double Age=(UtcNow()-Item.CreatedUtc).GetTotalSeconds();
    if (Age<0 || Age>=Policy.MaxAgeSeconds) Terminal=TEXT("QueueExpired");
    else if (!SameOrigin(Current->BaseUrl,Item.Url)) Terminal=TEXT("QueueOrigin");
    else if (!RequestIdentityScope(*Current,Owner->GetSessionData(),Item.AuthMode,Item.Headers,Scope,&Owner->GetManagedLifetime()) || Scope!=Item.Scope)
        Terminal=TEXT("QueueIdentity");
	if (Owner->GetManagedLifetime().IsValid())
	{
		// The clock/identity calls above may reenter teardown. No new Send follows a revoked binding or observed Stop.
		const auto Binding=Owner->QueueBinding;
		const auto& Native=Owner->GetManagedLifetime();
		if (bStopped || Native.Control->IsStopped() || Native.Gate->IsClosed() || !Binding ||
			!Binding->AllowsEnqueue() || !Members.Contains(Binding->GetKey()))
		{ bReplayActive=false; bProbeReplay=false; return false; }
		if (NextReplayId==0)
		{ bStorageFailed=true; bReplayActive=false; bProbeReplay=false; return false; }
		const uint64 Id=NextReplayId;
		NextReplayId=NextReplayId==MAX_uint64 ? 0 : NextReplayId+1;
		const auto Op=MakeShared<FQueueReplayOperation,ESPMode::ThreadSafe>(AsShared(),Id,Binding->GetKey(),Item,bProbe,Terminal,ProbeToken);
		ManagedReplay=Op; // before Send can synchronously emit Started, complete or detach this owner
		const auto Handle=FRequestSpecAccess::SendQueued(Owner.ToSharedRef(),Item,bProbe,{},Terminal,Op);
		if (Handle.GetId()==0 && !Op->Managed.IsValid()) Op->AdmissionRefusedOnGT();
		return true;
	}
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

void VaRest::Http::FOfflineQueue::ReleaseManagedReplay(const TSharedRef<FQueueReplayOperation,ESPMode::ThreadSafe>& Op,bool bNormal)
{
	check(IsInGameThread());
	if (ManagedReplay!=Op) return;
	const auto Keep=AsShared();
	Op->Phase=FQueueReplayOperation::EPhase::Settled;
	ManagedReplay.Reset();
	bReplayActive=false; bProbeReplay=false;
	ReplayProbeToken={};
	if (bNormal && !bStopped && !FClientState::IsShuttingDown()) Op->CompleteProbeOnGT();
	Op->Queue.Reset(); // break ONLY this completed operation's owner cycle; no completed/history list
}

void VaRest::Http::FOfflineQueue::DiscardManagedReplay(const TSharedRef<FQueueReplayOperation,ESPMode::ThreadSafe>& Op)
{
	check(IsInGameThread());
	if (ManagedReplay!=Op) { Op->Queue.Reset(); return; }
	// Actual stopped anchor cleanup. No successful terminal, tombstone, clock/service call or user notification.
	bStorageFailed=true;
	ReleaseManagedReplay(Op,false);
}

void VaRest::Http::FOfflineQueue::AdvanceManagedReplay(const TSharedRef<FQueueReplayOperation,ESPMode::ThreadSafe>& Op)
{
	check(IsInGameThread());
	const auto Keep=AsShared();
	if (ManagedReplay!=Op || Op->ReplayId==0 || bStopped || FClientState::IsShuttingDown()) return;
	if (!Op->bManagedSettled || (!Op->bAdmissionRefused && (!Op->bCoreTerminal || !Op->bTraceReturned))) return;
	if (Op->Phase==FQueueReplayOperation::EPhase::RetireIo || Op->Phase==FQueueReplayOperation::EPhase::Settled) return;
	if (Items.IsEmpty() || Items[0].Sequence!=Op->Sequence || !Items[0].QueueId.Equals(Op->QueueId,ESearchCase::CaseSensitive))
	{ bStorageFailed=true; ReleaseManagedReplay(Op,true); return; }
	if (Op->Disposition!=FQueueReplayOperation::EDisposition::Retire)
	{
		const bool bBackoff=CoordinatorPhase==ECoordinatorPhase::Live &&
			(Op->Disposition==FQueueReplayOperation::EDisposition::RetainNetwork || Op->bAdmissionRefused);
		if (bBackoff)
		{
		if (CoordinatorPhase==ECoordinatorPhase::Live) ReplayNotBefore=QueueClock->Now()+10.0;
			if (bStopped || FClientState::IsShuttingDown()) { ReleaseManagedReplay(Op,false); return; }
			QueueClock->ClearTimer(ReplayRetryTimer);
		}
		ReleaseManagedReplay(Op,true);
		if (bStopped || FClientState::IsShuttingDown()) return;
		if (!bBackoff) { ScheduleReplay(); return; } // expected detach cancellation does not manufacture network failure
		const TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe> Weak(Keep);
		if (!bStopped && !FClientState::IsShuttingDown())
			ReplayRetryTimer=QueueClock->SetTimer(10.0,[Weak]{ if (const auto Self=Weak.Pin()) Self->ScheduleReplay(); });
		return;
	}
	check(Op->Result.IsSet());
	Op->Phase=FQueueReplayOperation::EPhase::RetireIo;
	// This exact accepted tombstone survives member departure and persistence's normal shutdown suppression.
	Op->bRetireIoAdmitted=FQueueStore::RetireOwned(Directory,Op->Sequence,Op->QueueId,
		[Keep,Op](bool bOk){ Keep->FinishManagedRetire(Op,bOk); });
}

void VaRest::Http::FOfflineQueue::FinishManagedRetire(const TSharedRef<FQueueReplayOperation,ESPMode::ThreadSafe>& Op,bool bOk)
{
	check(IsInGameThread());
	const auto Keep=AsShared();
	if (ManagedReplay!=Op || Op->Phase!=FQueueReplayOperation::EPhase::RetireIo) return;
	if (bOk && !Items.IsEmpty() && Items[0].Sequence==Op->Sequence && Items[0].QueueId.Equals(Op->QueueId,ESearchCase::CaseSensitive))
	{
		CommittedBodyBytes-=Items[0].Body.Bytes.Num();
		CommittedMetadataBytes-=QueueMetadataCharge(Items[0]);
		Items.RemoveAt(0); // actual successful IO governs removal even after the originating member detached
	}
	else { bOk=false; bStorageFailed=true; }
	FVaRestQueuedResult Result=MoveTemp(Op->Result.GetValue());
	Op->Result.Reset();
	ReleaseManagedReplay(Op,!bStopped && !FClientState::IsShuttingDown());
	if (bOk && !bStopped && !FClientState::IsShuttingDown())
	{ BroadcastLater(MoveTemp(Result)); ScheduleReplay(); }
}
void VaRest::Http::FOfflineQueue::ReplayFinished(FString QueueId,FVaRestQueuedResult Result,bool bKeep)
{
    LLM_SCOPE_BYTAG(VaRest);
    if (bKeep)
    {
        bReplayActive=false;
        const bool bWasProbe=bProbeReplay; bProbeReplay=false;
		const auto Token=ReplayProbeToken; ReplayProbeToken={};
		// Install retention before completing the probe can synchronously arm another owned candidate.
		ReplayNotBefore=QueueClock->Now()+10.0;
        if (const auto Reachability=Connectivity.Pin())
        {
			if (bWasProbe)
			{
				if (Token.IsValid()) Reachability->CompleteOwnedProbe(Token);
				else Reachability->ProbeFinished();
			}
        }
        // Logical Connection is not physical evidence. Pause this queue, not shared reachability.
		if (CoordinatorPhase==ECoordinatorPhase::Live) QueueClock->ClearTimer(ReplayRetryTimer);
        const TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe> Weak(AsShared());
        if (CoordinatorPhase==ECoordinatorPhase::Live && !bStopped && !FClientState::IsShuttingDown())
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
			const auto Token=Self->ReplayProbeToken; Self->ReplayProbeToken={};
            if (bWasProbe)
				if (const auto Reachability=Self->Connectivity.Pin())
				{
					if (Token.IsValid()) Reachability->CompleteOwnedProbe(Token);
					else Reachability->ProbeFinished();
				}
            if (bOk) { Self->BroadcastLater(MoveTemp(Result)); Self->ScheduleReplay(); }
        });
}
void VaRest::Http::FOfflineQueue::BroadcastLater(FVaRestQueuedResult Result)
{
    LLM_SCOPE_BYTAG(VaRest);
    // Called only from the asynchronous retirement handback: already later than Send/network completion.
    if (bStopped || FClientState::IsShuttingDown()) return;
    const auto Keep=AsShared();
	TArray<FQueueMemberKey> Keys; Members.GetKeys(Keys);
	Keys.Sort([](const FQueueMemberKey& A,const FQueueMemberKey& B)
	{ return A.ClientId<B.ClientId || (A.ClientId==B.ClientId && A.Epoch<B.Epoch); });
    TArray<TSharedPtr<FClientState,ESPMode::ThreadSafe>> Owners;
	for (const auto& Key : Keys)
	{
		const auto& Entry=Members.FindChecked(Key);
		if (const auto Member=Entry.Client.Pin())
			if (Member->Queue.Get()==this && Member->QueueBinding==Entry.Binding && Entry.Binding->AllowsEnqueue()) Owners.Add(Member);
	}
    for (const auto& Member : Owners)
    { VAREST_LLM_SCOPE_SENDER(Member->EventSenderTags); Member->QueuedResult.Broadcast(Result); }
}

bool VaRest::Http::FOfflineQueue::Flush(TUniqueFunction<void(bool)>&& Done,TSharedPtr<FQueueBinding,ESPMode::ThreadSafe> OriginBinding)
{
    LLM_SCOPE_BYTAG(VaRest);
    check(IsInGameThread());
    if (CoordinatorPhase!=ECoordinatorPhase::Live || bStopped || FClientState::IsShuttingDown() || FlushWaiters.Num()>=4096) return false;
    auto Waiter=MakeShared<FFlushWaiter,ESPMode::ThreadSafe>();
    Waiter->Through=LastAdmission;
    Waiter->Done=MoveTemp(Done);
	Waiter->OriginBinding=MoveTemp(OriginBinding);
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
    if (Waiter->bDelivering) return false;
    if (bStopped || FClientState::IsShuttingDown()) { FlushWaiters.Remove(Waiter); return true; }
    if (Waiter->Result.IsSet())
    {
        const bool bOk=Waiter->Result.GetValue();
		Waiter->bDelivering=true;
        auto Callback=MoveTemp(Waiter->Done);
        if (Callback) Callback(bOk);
		Callback.Reset(); // complete real payload destruction before this member's native drain can observe readiness
		FlushWaiters.Remove(Waiter);
        return true;
    }
    LLM_SCOPE_BYTAG(VaRest);
    if (!bLoaded || Waiter->bBarrierStarted) return false;
    for (const auto& Ticket : ActiveAdmissions) if (Ticket.Key<=Waiter->Through) return false;
    Waiter->bBarrierStarted=true;
    if (!LaunchPersistence([]{ FIoResult Result; Result.bOk=true; return Result; },
        [Self=AsShared(),Waiter](FIoResult&& Result)
        { Waiter->Result=Result.bOk && !Self->bStorageFailed && !Self->bFlushFailed; }))
        Waiter->Result=false;
    return false; // both refusal and worker success are delivered by a subsequent owned poll
}
