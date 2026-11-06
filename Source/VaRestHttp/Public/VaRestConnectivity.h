// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestClock.h"

struct FVaRestAttemptOutcome;
struct FVaRestProbeToken
{
	uint64 Generation=0;
	bool IsValid() const { return Generation!=0; }
};
using FVaRestConnectivityChangedEvent = TMulticastDelegate<void(bool)>;

namespace VaRest::Http::ConnectivityPrivate
{
    void PollOverride();
}

/** Game-thread reachability scope. A received HTTP error still proves reachability. */
class VARESTHTTP_API FVaRestConnectivity : public TSharedFromThis<FVaRestConnectivity, ESPMode::ThreadSafe>
{
public:
    static TSharedRef<FVaRestConnectivity, ESPMode::ThreadSafe> GetDefault();
    static TSharedRef<FVaRestConnectivity, ESPMode::ThreadSafe> Make(TSharedRef<IVaRestClock, ESPMode::ThreadSafe> Clock);
    ~FVaRestConnectivity();
    bool IsOnline() const;
    void ReportOnline(uint64 CauseRequestId = 0);
    void ReportOffline(uint64 CauseRequestId = 0);
    /** Synchronous GT observers, FIFO and nonrecursive. State changes before notification; request callbacks stay deferred. */
    FVaRestConnectivityChangedEvent& OnChanged() { return Changed; }
    void SetProbeInterval(double Seconds);
    /** Queue candidates across the scope precede fallback candidates; two-argument registrations remain preferred. */
    void RegisterProbe(uint64 ClientId, TFunction<bool()>&& TryStart, TFunction<bool()>&& TryFallback = {});
	void RegisterOwnedProbe(uint64 ClientId,TFunction<bool(FVaRestProbeToken)>&& TryStart,
		TFunction<bool(FVaRestProbeToken)>&& TryFallback={});
    void UnregisterProbe(uint64 ClientId);
    void ProbeFinished();
	bool CompleteOwnedProbe(FVaRestProbeToken Token);

private:
    friend void VaRest::Http::ConnectivityPrivate::PollOverride();
    /** Core cvar sink: publish effective state without overwriting the last network observation. */
    void RefreshOverride();
    explicit FVaRestConnectivity(TSharedRef<IVaRestClock, ESPMode::ThreadSafe> InClock);
    void Transition(bool bNewOnline, uint64 CauseRequestId);
    void PublishEffective(uint64 CauseRequestId, FString Reason);
    void ArmProbe();
    void RunProbe();
    TSharedRef<IVaRestClock, ESPMode::ThreadSafe> TimerClock;
    FVaRestTimerHandle ProbeTimer;
    FVaRestConnectivityChangedEvent Changed;
    struct FProbeCandidates
    {
        TFunction<bool()> Preferred,Fallback;
		TFunction<bool(FVaRestProbeToken)> OwnedPreferred,OwnedFallback;
    };
    TMap<uint64, FProbeCandidates> Probes;
    bool bOnline = true;
    bool bPublishedOnline = true;
    bool bDelivering = false;
    bool bProbeRunning = false;
	bool bActiveProbeOwned=false;
	uint64 NextProbeGeneration=1,ActiveProbeGeneration=0;
    double IntervalSeconds = 10.0;
    TArray<TTuple<bool, uint64, FString>> PendingEdges;
};

namespace VaRest::Http
{
    FVaRestAttemptOutcome MakeOfflineOutcome();
}
