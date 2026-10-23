// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestConnectivity.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Tasks/Task.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestTracePoints.h"
#include "VaRestStage.h"

namespace VaRest::Http::ConnectivityPrivate
{
    TArray<TWeakPtr<FVaRestConnectivity, ESPMode::ThreadSafe>>& Live()
    {
        static auto* Values = new TArray<TWeakPtr<FVaRestConnectivity, ESPMode::ThreadSafe>>();
        return *Values;
    }
#if !UE_BUILD_SHIPPING
    TAutoConsoleVariable<int32> Force(TEXT("VaRest.Http.ForceOffline"), 0,
        TEXT("Force effective HTTP connectivity offline; preserve observed state"));
#endif
    bool Forced()
    {
#if !UE_BUILD_SHIPPING
        return Force.GetValueOnGameThread() != 0;
#else
        return false;
#endif
    }
    void PollOverride()
    {
        if (!IsInGameThread())
        {
            UE::Tasks::Launch(TEXT("VaRest.ConnectivityOverride"), [] { PollOverride(); },
                UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
            return;
        }
        auto& Values = Live();
        Values.RemoveAll([](const auto& Weak) { return !Weak.IsValid(); });
        const auto Snapshot = Values;
        for (const auto& Weak : Snapshot)
            if (const auto Pinned = Weak.Pin()) Pinned->RefreshOverride();
    }
#if !UE_BUILD_SHIPPING
    FAutoConsoleVariableSink Sink(FConsoleCommandDelegate::CreateStatic(&PollOverride));
#endif
    bool HintPermitsProbe()
    {
        ENetworkConnectionType Hint = ENetworkConnectionType::Unknown;
#if WITH_DEV_AUTOMATION_TESTS
        const auto Injected = Testing::GetConnectivityHint();
        if (Injected.IsSet()) Hint = Injected.GetValue();
        else
#endif
        {
#if PLATFORM_ANDROID || PLATFORM_IOS
            Hint = FPlatformMisc::GetNetworkConnectionType();
#endif
        }
        return Hint != ENetworkConnectionType::None && Hint != ENetworkConnectionType::AirplaneMode;
    }
}

FVaRestConnectivity::FVaRestConnectivity(TSharedRef<IVaRestClock, ESPMode::ThreadSafe> InClock)
    : TimerClock(MoveTemp(InClock)) {}
FVaRestConnectivity::~FVaRestConnectivity()
{
    check(IsInGameThread());
    TimerClock->ClearTimer(ProbeTimer);
    VaRest::Http::ConnectivityPrivate::Live().RemoveAll([this](const auto& Weak)
    { return !Weak.IsValid() || Weak.HasSameObject(this); });
}
TSharedRef<FVaRestConnectivity, ESPMode::ThreadSafe> FVaRestConnectivity::Make(TSharedRef<IVaRestClock, ESPMode::ThreadSafe> Clock)
{
    check(IsInGameThread());
    TSharedRef<FVaRestConnectivity, ESPMode::ThreadSafe> Value = MakeShareable(new FVaRestConnectivity(MoveTemp(Clock)));
    VaRest::Http::ConnectivityPrivate::Live().Add(Value);
    Value->bPublishedOnline = Value->IsOnline();
    return Value;
}
TSharedRef<FVaRestConnectivity, ESPMode::ThreadSafe> FVaRestConnectivity::GetDefault()
{
    static auto* Value = new TSharedRef<FVaRestConnectivity, ESPMode::ThreadSafe>(Make(FVaRestRealClock::Get()));
    return *Value;
}
bool FVaRestConnectivity::IsOnline() const
{
    check(IsInGameThread());
    return bOnline && !VaRest::Http::ConnectivityPrivate::Forced();
}
void FVaRestConnectivity::ReportOnline(uint64 CauseRequestId) { Transition(true, CauseRequestId); }
void FVaRestConnectivity::ReportOffline(uint64 CauseRequestId) { Transition(false, CauseRequestId); }
void FVaRestConnectivity::Transition(bool bNewOnline, uint64 CauseRequestId)
{
    check(IsInGameThread());
    bOnline = bNewOnline;
    PublishEffective(CauseRequestId, bNewOnline ? TEXT("ObservedOnline") : TEXT("ObservedOffline"));
}
void FVaRestConnectivity::RefreshOverride()
{
    PublishEffective(0, VaRest::Http::ConnectivityPrivate::Forced() ? TEXT("OverrideEnabled") : TEXT("OverrideRemoved"));
}
void FVaRestConnectivity::PublishEffective(uint64 CauseRequestId, FString Reason)
{
    check(IsInGameThread());
    const auto Keep = AsShared();
    const bool bEffective = IsOnline();
    if (bEffective != bPublishedOnline)
    {
        bPublishedOnline = bEffective;
        PendingEdges.Emplace(bEffective, CauseRequestId, MoveTemp(Reason));
    }
    if (bDelivering) return;
    TGuardValue<bool> Delivering(bDelivering, true);
    for (int32 Index = 0; Index < PendingEdges.Num(); ++Index)
    {
        const auto Edge = PendingEdges[Index];
        VaRest::Http::FClientState::ForEachLive([this, &Edge](VaRest::Http::FClientState& Client)
        {
            if (Client.GetConfig()->Connectivity.Get() != this) return;
            VaRest::Http::TracePoints::Marker(Client, Edge.Get<0>() ? EVaRestTraceMarkerKind::Online : EVaRestTraceMarkerKind::Offline,
                Edge.Get<1>(), Edge.Get<2>());
        });
        Changed.Broadcast(Edge.Get<0>());
    }
    const bool bHadEdges = !PendingEdges.IsEmpty();
    PendingEdges.Reset();
    // Also reconcile a force change that masks an already-offline observation (no new edge).
    if (bHadEdges || VaRest::Http::ConnectivityPrivate::Forced() || !ProbeTimer.IsValid()) ArmProbe();
}
void FVaRestConnectivity::SetProbeInterval(double Seconds)
{
    check(IsInGameThread());
    IntervalSeconds = FMath::IsFinite(Seconds) ? FMath::Clamp(Seconds, 0.1, 3600.0) : 10.0;
    ArmProbe();
}
void FVaRestConnectivity::RegisterProbe(uint64 ClientId, TFunction<bool()>&& TryStart)
{
    check(IsInGameThread());
    Probes.Add(ClientId, MoveTemp(TryStart));
    ArmProbe();
}
void FVaRestConnectivity::UnregisterProbe(uint64 ClientId)
{
    check(IsInGameThread());
    Probes.Remove(ClientId);
    ArmProbe();
}
void FVaRestConnectivity::ArmProbe()
{
    TimerClock->ClearTimer(ProbeTimer);
    if (IsOnline() || VaRest::Http::ConnectivityPrivate::Forced() || bProbeRunning || Probes.IsEmpty()) return;
    const TWeakPtr<FVaRestConnectivity, ESPMode::ThreadSafe> Weak(AsShared());
    ProbeTimer = TimerClock->SetTimer(IntervalSeconds, [Weak]
    { if (const auto Pinned = Weak.Pin()) Pinned->RunProbe(); });
}
void FVaRestConnectivity::RunProbe()
{
    ProbeTimer.Invalidate();
    if (IsOnline() || bProbeRunning) return;
    if (VaRest::Http::ConnectivityPrivate::Forced()) return;
    if (!VaRest::Http::ConnectivityPrivate::HintPermitsProbe()) { ArmProbe(); return; }
    TArray<uint64> Owners;
    Probes.GetKeys(Owners);
    Owners.Sort();
    for (uint64 OwnerId : Owners)
    {
        const TFunction<bool()>* Probe = Probes.Find(OwnerId);
        if (!Probe || !*Probe) continue;
        TFunction<bool()> TryStart = *Probe; // owns captures across unregister/reconfigure during Send/trace/events
        bProbeRunning = true;
        if (TryStart()) return;
        bProbeRunning = false;
        if (IsOnline() || VaRest::Http::ConnectivityPrivate::Forced()) break;
    }
    ArmProbe();
}
void FVaRestConnectivity::ProbeFinished()
{
    check(IsInGameThread());
    bProbeRunning = false;
    ArmProbe();
}
FVaRestAttemptOutcome VaRest::Http::MakeOfflineOutcome()
{
    FVaRestAttemptOutcome Result;
    Result.Failure = EVaRestTransportFailure::Other;
    Result.FailureDetail = TEXT("Offline");
    Result.FailureMessage = TEXT("connectivity is offline");
    return Result;
}
