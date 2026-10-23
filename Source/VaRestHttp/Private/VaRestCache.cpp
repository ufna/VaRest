// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestCache.h"
#include "VaRestClientState.h"
#include "VaRestPersistenceWait.h"
#include "VaRestClassify.h"
#include "VaRestTracePoints.h"
#include "VaRestCachePolicy.h"
#include "VaRestConnectivity.h"
#include "VaRestHttpStatsInternal.h"
#include "VaRestIdentity.h"
#include "VaRestRetryInternal.h"

namespace VaRest::Http::CachePrivate
{
// Retained metadata is representation metadata, not the original wire headers. Only
// key rules run here; all secret-value/body scans remain in the disk worker.
FVaRestHeaders RetainedHeaders(const FExchangeState& Ex,const FVaRestHeaders& Input,int64 BodyBytes)
{
    const auto Current=Ex.State->GetConfig()->PersistenceGuard;
    const auto Normalized=MergeNotModified(Input,FVaRestHeaders(),BodyBytes,Ex.Verb==TEXT("HEAD"));
    FVaRestHeaders Result;
    for (const auto& Pair : Normalized)
        if (!Ex.PersistenceGuard->Rules.ShouldRedact(Pair.Key,EVaRestRedactKeyKind::Header)
            && !Current->Rules.ShouldRedact(Pair.Key,EVaRestRedactKeyKind::Header)) Result.Add(Pair.Key,Pair.Value);
    return Result;
}
void KeepRetainedValidators(FCacheEntry& Stored)
{
    Stored.Metadata.ETag=Stored.Headers.Get(TEXT("ETag"));
    Stored.Metadata.LastModified=Stored.Headers.Get(TEXT("Last-Modified"));
}

// Answer headers are deliberately lossy. Policy is carried independently, and new
// restrictions are read from the raw304 before hop/protected names are removed.
FCacheMetadata RevalidatedPolicy(const FCacheEntry& Stored,const FVaRestHeaders& Raw,double Now,FDateTime Utc)
{
    FCacheMetadata Result=Stored.Metadata;
    const auto Fresh=ReadCacheMetadata(Raw,0,Now,Utc);
    Result.bNoStore|=Fresh.bNoStore;
    if (Raw.Contains(TEXT("Vary"))) Result.Vary=Fresh.Vary;
    if (!Raw.Contains(TEXT("Cache-Control")))
    {
        // With no freshness update retain the original age anchor, not a new default TTL.
        // Date/Age/Expires alone cannot establish precedence over a potentially filtered CC.
        if (Raw.Contains(TEXT("Date")) || Raw.Contains(TEXT("Age")) || Raw.Contains(TEXT("Expires"))) Result.bNoStore=true;
        return Result;
    }
    // Missing stored CC is unknown provenance, even when its parsed lifetime equals
    // the client default. Do not reconstruct it from the retained header map.
    if (!Stored.Headers.Contains(TEXT("Cache-Control"))) Result.bNoStore=true;
    Result.bNoCache=Fresh.bNoCache;
    Result.bMustRevalidate=Fresh.bMustRevalidate;
    // Different sentinel defaults reveal a missing explicit lifetime without adding
    // another parser. no-cache is safe to retain: it permits neither fresh nor stale reuse.
    if (Fresh.Lifetime!=ReadCacheMetadata(Raw,1,Now,Utc).Lifetime && !Fresh.bNoCache) Result.bNoStore=true;
    Result.Lifetime=Fresh.Lifetime;
    Result.StoredAt=Now; Result.StoredUtc=Utc; Result.InitialAge=Fresh.InitialAge;
    // Only an entirely valid explicit Age can replace the inherited floor. A
    // malformed duplicate must not erase it merely because another value parses.
    bool bValidAge=Raw.Contains(TEXT("Age"));
    for (const auto& Pair : Raw)
    {
        if (!Pair.Key.Equals(TEXT("Age"),ESearchCase::IgnoreCase)) continue;
        double ParsedAge=0.0;
        if (!ReadDeltaSeconds(Pair.Value.TrimStartAndEnd(),ParsedAge)) bValidAge=false;
    }
    // A new lifetime or a malformed Date cannot erase an inherited age anchor.
    FDateTime FreshDate;
    if (!FDateTime::ParseHttpDate(Raw.Get(TEXT("Date")),FreshDate))
        Result.InitialAge=FMath::Max(Result.InitialAge,Stored.Metadata.InitialAge+FMath::Max(0.0,Now-Stored.Metadata.StoredAt));
    else if (!bValidAge) Result.InitialAge=FMath::Max(Result.InitialAge,Stored.Metadata.InitialAge);
    return Result;
}

class FCacheOperation : public TSharedFromThis<FCacheOperation,ESPMode::ThreadSafe>
{
public:
    FCacheOperation(FVaRestExchangeRef InExchange,FVaRestStageNext InNext,FVaRestOutcomeCallback&& InDone)
        : Exchange(MoveTemp(InExchange)),Next(MoveTemp(InNext)),Done(MoveTemp(InDone)) {}
    void Start();
    void AfterLookup(TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Candidate,bool bDisk);
    void Proceed();
    void OnResult(FVaRestAttemptOutcome&& Outcome);
    void Complete(FVaRestAttemptOutcome&& Outcome);
    void ServeStale();
    bool CanContinue();
private:
    bool bSettled=false;
    FVaRestExchangeRef Exchange;
    FVaRestStageNext Next;
    FVaRestOutcomeCallback Done;
    TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Entry;
    TSharedPtr<FDiskCache,ESPMode::ThreadSafe> Disk;
    TSharedPtr<FPersistenceWait,ESPMode::ThreadSafe> Wait;
    FString Key,Identity;
    uint64 Epoch=0;
    bool bForceValidation=false;
};
void FCacheOperation::Complete(FVaRestAttemptOutcome&& Outcome)
{
    if (bSettled) return;
    bSettled=true;
    if (Wait.IsValid()) { Wait->Finish(MoveTemp(Outcome)); Wait.Reset(); return; }
    auto Callback=MoveTemp(Done);
    if (Callback) Callback(MoveTemp(Outcome));
}
void FCacheOperation::Proceed()
{
    if (!CanContinue()) return;
    if (Wait.IsValid()) { Done=Wait->Resume(); Wait.Reset(); }
    if (!Done) return;
    Next.Proceed(Exchange,[Self=AsShared()](FVaRestAttemptOutcome&& Outcome)
    { Self->OnResult(MoveTemp(Outcome)); });
}
void FCacheOperation::ServeStale()
{
    auto& Ex=Internals(*Exchange);
    if (!CanContinue()) return;
    Ex.CacheLookup=EVaRestTraceCacheLookup::Stale;
    if (!Ex.bCacheHitCounted) { Ex.bCacheHitCounted=true; FHttpStats::CacheHit(Ex.State->GetCounters()); }
    FVaRestTraceNote Note; Note.Kind=EVaRestTraceNoteKind::CacheServedStale;
    TracePoints::Note(Ex,MoveTemp(Note));
    auto Answer=Entry->Answer(); Answer.EffectiveUrl=Ex.Url;
    if (!CanContinue()) return;
    Complete(MoveTemp(Answer));
}
void FCacheOperation::Start()
{
    auto& Ex=Internals(*Exchange);
    if (!CanContinue()) return;
    Ex.bCacheLookupPending=false;
    if (!BuildCacheKey(Ex,Key,Identity))
    {
        if (!Ex.Config->Connectivity->IsOnline() && !Ex.bConnectivityProbe && !Ex.bIsAuthRefresh)
            Complete(MakeOfflineOutcome());
        else Next.Proceed(Exchange,MoveTemp(Done));
        return;
    }
    Ex.LookupKey=Key; Ex.LookupIdentity=Identity;
    Epoch=Ex.State->GetCacheEpoch(); Ex.CacheEpoch=Epoch;
    if (!Ex.bCacheLookupCounted) { Ex.bCacheLookupCounted=true; FHttpStats::CacheLookup(Ex.State->GetCounters()); }
    const auto CheckValidation=[this](const FVaRestHeaders& Headers)
    {
        for (const auto& Pair : Headers)
        {
            if (!Pair.Key.Equals(TEXT("Cache-Control"),ESearchCase::IgnoreCase)) continue;
            const FString Lower=Pair.Value.ToLower();
            bForceValidation|=Lower.Contains(TEXT("no-cache")) || Lower.Contains(TEXT("max-age=0"));
        }
    };
    CheckValidation(Ex.RequestHeaders);
    if (Ex.AuthMode!=EVaRestAuthMode::None) CheckValidation(Ex.State->GetSessionData().Headers);
    auto Memory=Ex.State->GetMemoryCache().Find(Key);
    if (Memory.IsValid()) { AfterLookup(MoveTemp(Memory),false); return; }
    if (Ex.CachePolicy.Mode!=EVaRestCacheMode::MemoryAndDisk) { AfterLookup(nullptr,false); return; }
    Ex.bCacheLookupPending=true;
    Wait=FPersistenceWait::Begin(Exchange,MoveTemp(Done));
    if (!Wait->IsPending()) return;
    Disk=Ex.State->GetDiskCacheFor(*Ex.Config,Ex.CachePolicy.DiskBytes);
    const bool bAccepted=Disk->Find(Key,Ex.Config->Clock->Now(),FDateTime::UtcNow(),
        [Self=AsShared()](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Candidate)
        { Self->AfterLookup(MoveTemp(Candidate),true); });
    if (!bAccepted)
    {
        if (PersistenceAccepting()) AfterLookup(nullptr,true);
        else
        {
            FVaRestAttemptOutcome Refused;
            Refused.Failure=EVaRestTransportFailure::Other;
            Refused.FailureDetail=TEXT("PersistenceStopped");
            Complete(MoveTemp(Refused));
        }
    }
}
void FCacheOperation::OnResult(FVaRestAttemptOutcome&& Outcome)
{
    auto& Ex=Internals(*Exchange);
    if (!CanContinue()) return;
    FString CurrentKey,CurrentIdentity;
    const bool bSame=!Ex.bCacheIdentityChanged && BuildCacheKey(Ex,CurrentKey,CurrentIdentity)
        && CurrentKey==Key && CurrentIdentity==Identity && Epoch==Ex.State->GetCacheEpoch();
    if (!bSame) { Complete(MoveTemp(Outcome)); return; }
    FVaRestHeaders VaryHeaders=Ex.RequestHeaders;
    if (Ex.bCacheConditional) { VaryHeaders.Remove(TEXT("If-None-Match")); VaryHeaders.Remove(TEXT("If-Modified-Since")); }
    if (Ex.AuthMode!=EVaRestAuthMode::None)
        for (const auto& Header : Ex.State->GetSessionData().Headers) VaryHeaders.Set(Header.Key,Header.Value);
    const auto Kind=ClassifyAttempt(Outcome).Kind;
    if ((Kind==EVaRestErrorKind::Connection || Kind==EVaRestErrorKind::Offline)
        && Entry.IsValid() && Ex.CachePolicy.bServeStaleOnError && !bForceValidation && Entry->Metadata.CanServeStale())
    { ServeStale(); return; }
    if (Outcome.Status==304 && !Outcome.IsTransportFailure() && Entry.IsValid() && Ex.bCacheConditional)
    {
        FCacheEntry Updated=*Entry;
        Updated.Generation=FGuid::NewGuid();
        Updated.Headers=MergeNotModified(Entry->Headers,Outcome.Headers,Entry->Body.Num(),Ex.Verb==TEXT("HEAD"));
        if (Updated.Headers.Contains(TEXT("Content-Type"))) Updated.ContentType=Updated.Headers.Get(TEXT("Content-Type"));
        Updated.Metadata=RevalidatedPolicy(*Entry,Outcome.Headers,Ex.Config->Clock->Now(),FDateTime::UtcNow());
        Updated.Headers=RetainedHeaders(Ex,Updated.Headers,Updated.Body.Num());
        KeepRetainedValidators(Updated);
        if (!CanContinue()) return;
        Ex.State->GetMemoryCache().Remove(Key);
        if (Ex.CachePolicy.Mode==EVaRestCacheMode::MemoryAndDisk)
            Ex.State->GetDiskCacheFor(*Ex.Config,Ex.CachePolicy.DiskBytes)->Remove(Key);
        if (!Updated.Metadata.bNoStore && CacheHeadersEligible(VaryHeaders,Outcome.Headers) && CacheHeadersEligible(VaryHeaders,Updated.Headers))
        {
            if (!CanContinue()) return;
            Ex.State->GetMemoryCache().Put(Updated,Ex.CachePolicy.MemoryBytes);
            if (Ex.CachePolicy.Mode==EVaRestCacheMode::MemoryAndDisk)
            {
                FPersistenceGuard Guard=*Ex.PersistenceGuard; Guard.UnionWith(*Ex.State->GetConfig()->PersistenceGuard); Guard.AbortSignal=Ex.PersistenceAbort;
                if (!CanContinue()) return;
                Ex.State->GetDiskCacheFor(*Ex.Config,Ex.CachePolicy.DiskBytes)->Put(Updated,MoveTemp(Guard));
            }
        }
        if (!CanContinue()) return;
        FVaRestTraceNote Note; Note.Kind=EVaRestTraceNoteKind::CacheRevalidated;
        TracePoints::Note(Ex,MoveTemp(Note));
        if (!Ex.bCacheHitCounted) { Ex.bCacheHitCounted=true; FHttpStats::CacheHit(Ex.State->GetCounters()); }
        auto Answer=Updated.Answer(); Answer.EffectiveUrl=Ex.Url;
        Answer.BytesUp=Outcome.BytesUp; Answer.BytesDown=Outcome.BytesDown;
        if (!CanContinue()) return;
        Complete(MoveTemp(Answer)); return;
    }
    if (Outcome.Status==200 && !Outcome.IsTransportFailure() && !Outcome.bBodyInFile)
    {
        const auto Meta=ReadCacheMetadata(Outcome.Headers,Ex.CachePolicy.DefaultTtlSeconds,Ex.Config->Clock->Now(),FDateTime::UtcNow());
        if (Meta.bNoStore)
        {
            Ex.State->GetMemoryCache().Remove(Key);
            if (Ex.CachePolicy.Mode==EVaRestCacheMode::MemoryAndDisk) Ex.State->GetDiskCacheFor(*Ex.Config,Ex.CachePolicy.DiskBytes)->Remove(Key);
        }
        else if (CacheHeadersEligible(VaryHeaders,Outcome.Headers))
        {
            FCacheEntry Stored;
            Stored.Key=Key; Stored.ContentType=Outcome.ContentType;
            if (Ex.Verb!=TEXT("HEAD")) Stored.Body=Outcome.Body;
            Stored.Headers=RetainedHeaders(Ex,Outcome.Headers,Stored.Body.Num());
            Stored.Metadata=Meta; KeepRetainedValidators(Stored);
            if (!CanContinue()) return;
            Ex.State->GetMemoryCache().Put(Stored,Ex.CachePolicy.MemoryBytes);
            if (Ex.CachePolicy.Mode==EVaRestCacheMode::MemoryAndDisk)
            {
                FPersistenceGuard Guard=*Ex.PersistenceGuard;
                Guard.UnionWith(*Ex.State->GetConfig()->PersistenceGuard); Guard.AbortSignal=Ex.PersistenceAbort;
                if (!CanContinue()) return;
                Ex.State->GetDiskCacheFor(*Ex.Config,Ex.CachePolicy.DiskBytes)->Put(Stored,MoveTemp(Guard));
            }
            if (Ex.State->GetMemoryCache().Find(Key).IsValid())
            {
                if (!CanContinue()) return;
                FVaRestTraceNote Note; Note.Kind=EVaRestTraceNoteKind::CacheStored; Note.Bytes=Stored.Body.Num();
                TracePoints::Note(Ex,MoveTemp(Note));
            }
        }
    }
    Complete(MoveTemp(Outcome));
}
class FCacheStage final : public IVaRestStage
{
public:
    virtual FName GetStageName() const override { return TEXT("Cache"); }
    virtual void Process(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done) override
    {
        auto Operation=MakeShared<FCacheOperation,ESPMode::ThreadSafe>(Exchange,Next,MoveTemp(Done));
        Operation->Start();
    }
};
// Every cache decision checks live time: timer delivery can lag the logical deadline.
// The callback is settled directly, never recursively fed through OnResult.
bool FCacheOperation::CanContinue()
{
    if (bSettled) return false;
    if (Wait.IsValid()) return Wait->CanHandoff();
    auto& Ex=Internals(*Exchange);
    if (IsOwnerGone(Ex)) MarkOwnerGone(Ex);
    if (Ex.bFinished || Ex.bCompletionQueued || Ex.Phase!=EVaRestExchangePhase::InChain || Ex.IsCancelRequested())
    {
        Complete(FVaRestAttemptOutcome::Canceled());
        return false;
    }
    if (Ex.DeadlineAt>0 && Ex.Config->Clock->Now()>=Ex.DeadlineAt-DeadlineTimerSlack)
    {
        Ex.PersistenceAbort->store(true,std::memory_order_relaxed);
        Complete(MakeDeadlineOutcome());
        return false;
    }
    return true;
}
void FCacheOperation::AfterLookup(TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Candidate, bool bDisk)
{
    FExchangeState& Ex = Internals(*Exchange);
    if (!CanContinue()) return;
    FString CurrentKey, CurrentIdentity;
    if (!BuildCacheKey(Ex, CurrentKey, CurrentIdentity) || CurrentKey != Key || CurrentIdentity != Identity
        || Epoch != Ex.State->GetCacheEpoch())
    {
        Candidate.Reset();
        Ex.bCacheIdentityChanged = true;
    }
    Ex.bCacheLookupPending = false;
    if (Candidate.IsValid() && Ex.Handler.IsValid() && Ex.Handler->DecodesJson()
        && Ex.MaxJsonBytes>0 && Candidate->Body.Num()>Ex.MaxJsonBytes) Candidate.Reset();
    Entry = MoveTemp(Candidate);
    if (Entry.IsValid() && Entry->Metadata.IsFresh(Ex.Config->Clock->Now()) && !bForceValidation)
    {
        Ex.CacheLookup = bDisk ? EVaRestTraceCacheLookup::HitDisk : EVaRestTraceCacheLookup::HitMemory;
        if (!Ex.bCacheHitCounted) { Ex.bCacheHitCounted=true; FHttpStats::CacheHit(Ex.State->GetCounters()); }
        FVaRestAttemptOutcome Answer = Entry->Answer();
        Answer.EffectiveUrl = Ex.Url;
        if (!CanContinue()) return;
        Complete(MoveTemp(Answer));
        return;
    }
    Ex.CacheLookup = Entry.IsValid() ? EVaRestTraceCacheLookup::Stale : EVaRestTraceCacheLookup::Miss;
    if (!Ex.Config->Connectivity->IsOnline() && !Ex.bConnectivityProbe)
    {
        if (Entry.IsValid() && Ex.CachePolicy.bServeStaleOnError && !bForceValidation && Entry->Metadata.CanServeStale()) ServeStale();
        else Complete(MakeOfflineOutcome());
        return;
    }
    if (Entry.IsValid() && (!Entry->Metadata.ETag.IsEmpty() || !Entry->Metadata.LastModified.IsEmpty()))
    {
        if (!Entry->Metadata.ETag.IsEmpty()) Ex.RequestHeaders.Set(TEXT("If-None-Match"), Entry->Metadata.ETag);
        else Ex.RequestHeaders.Set(TEXT("If-Modified-Since"), Entry->Metadata.LastModified);
        Ex.bCacheConditional = true;
        Ex.CacheLookup = EVaRestTraceCacheLookup::Revalidate;
    }
    Proceed();
}
}
bool VaRest::Http::BuildCacheKey(const FExchangeState& Ex,FString& OutKey,FString& OutIdentity)
{
    OutKey.Reset(); OutIdentity.Reset();
    if (Ex.CachePolicy.Mode==EVaRestCacheMode::None || Ex.bIsAuthRefresh || Ex.bConnectivityProbe
        || (Ex.Verb!=TEXT("GET") && Ex.Verb!=TEXT("HEAD")) || Ex.bCacheHadBody || !Ex.Body.IsEmpty() || !Ex.SaveTo.IsEmpty()) return false;
    for (const auto& Stage : Ex.Config->CustomStages) if (Stage.Value==EVaRestStageSlot::PerAttempt) return false;
    FVaRestHeaders Built=Ex.RequestHeaders;
    if (Ex.bCacheConditional) { Built.Remove(TEXT("If-None-Match")); Built.Remove(TEXT("If-Modified-Since")); }
    if (!CacheHeadersEligible(Built,FVaRestHeaders())) return false;
    const auto& Session=Ex.State->GetSessionData();
    if (Ex.AuthMode!=EVaRestAuthMode::None)
    {
        FVaRestHeaders KnownHeaders=Built;
        for (const auto& Header : Session.Headers) KnownHeaders.Set(Header.Key,Header.Value);
        if (!CacheHeadersEligible(KnownHeaders,FVaRestHeaders())) return false;
    }
    if (!AuthIdentity(Ex.Config->Auth.Get(),Ex.AuthMode,Session,OutIdentity)) return false;
    OutKey=CacheKey(Ex.Verb,Ex.Url,OutIdentity,Built,Ex.AuthMode==EVaRestAuthMode::None ? FVaRestHeaders() : Session.Headers);
    return true;
}
void VaRest::Http::CheckConditionalIdentity(FExchangeState& Ex)
{
    if (!Ex.bCacheConditional) return;
    FString CurrentKey,CurrentIdentity;
    if (BuildCacheKey(Ex,CurrentKey,CurrentIdentity) && CurrentKey==Ex.LookupKey && CurrentIdentity==Ex.LookupIdentity) return;
    Ex.RequestHeaders.Remove(TEXT("If-None-Match")); Ex.RequestHeaders.Remove(TEXT("If-Modified-Since"));
    Ex.AttemptHeaders.Remove(TEXT("If-None-Match")); Ex.AttemptHeaders.Remove(TEXT("If-Modified-Since"));
    Ex.bCacheConditional=false; Ex.bCacheIdentityChanged=true;
}
FVaRestStageRef VaRest::Http::MakeCacheStage() { return MakeShared<CachePrivate::FCacheStage,ESPMode::ThreadSafe>(); }
