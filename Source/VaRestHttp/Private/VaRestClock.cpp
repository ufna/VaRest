// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestClock.h"

#include "HAL/PlatformTime.h"

uint64 VaRest::Http::NewTimerId()
{
	// Trivially destructible: safe at static destruction (review I-4)
	static std::atomic<uint64> Next{ 1 };
	return Next.fetch_add(1, std::memory_order_relaxed);
}

TSharedRef<FVaRestRealClock, ESPMode::ThreadSafe> FVaRestRealClock::Get()
{
	static TSharedRef<FVaRestRealClock, ESPMode::ThreadSafe> Instance = MakeShared<FVaRestRealClock, ESPMode::ThreadSafe>();
	return Instance;
}

double FVaRestRealClock::Now() const
{
	return FPlatformTime::Seconds();
}

FVaRestTimerHandle FVaRestRealClock::SetTimer(double DelaySeconds, TUniqueFunction<void()>&& Callback)
{
	check(IsInGameThread());
	const uint64 Id = VaRest::Http::NewTimerId();
	TWeakPtr<FVaRestRealClock, ESPMode::ThreadSafe> Weak = AsShared();
	Pending.Add(Id, FTSTicker::GetCoreTicker().AddTicker(TEXT("VaRest.Timer"), static_cast<float>(FMath::Max(DelaySeconds, 0.0)),
		[Weak, Id, Fn = MoveTemp(Callback)](float) mutable
		{
			if (TSharedPtr<FVaRestRealClock, ESPMode::ThreadSafe> Clock = Weak.Pin())
			{
				Clock->Pending.Remove(Id);
			}
			Fn();
			return false; // one shot: the ticker drops it
		}));
	return FVaRestTimerHandle{ Id };
}

void FVaRestRealClock::ClearTimer(FVaRestTimerHandle& Handle)
{
	check(IsInGameThread());
	if (FTSTicker::FDelegateHandle* Ticker = Pending.Find(Handle.Id))
	{
		FTSTicker::RemoveTicker(*Ticker);
		Pending.Remove(Handle.Id);
	}
	Handle.Invalidate();
}

FVaRestTimerHandle FVaRestVirtualClock::SetTimer(double DelaySeconds, TUniqueFunction<void()>&& Callback)
{
	check(IsInGameThread());
	FTimer& Timer = Timers.AddDefaulted_GetRef();
	Timer.Id = VaRest::Http::NewTimerId();
	Timer.Due = Nanos.load(std::memory_order_acquire) + ToNanos(DelaySeconds);
	Timer.Callback = MoveTemp(Callback);
	return FVaRestTimerHandle{ Timer.Id };
}

void FVaRestVirtualClock::ClearTimer(FVaRestTimerHandle& Handle)
{
	check(IsInGameThread());
	Timers.RemoveAll([Id = Handle.Id](const FTimer& Timer) { return Timer.Id == Id; });
	Handle.Invalidate();
}

void FVaRestVirtualClock::Advance(double DeltaSeconds)
{
	check(IsInGameThread());
	const int64 Target = Nanos.load(std::memory_order_acquire) + ToNanos(DeltaSeconds);
	// Review N-2: a timer that re-arms itself at a zero delay fires forever at one instant. A few at one instant are
	// normal (Advance(0), a timer set while firing); MaxFiredAtOneInstant is that bug, so fail with a name, not a hang.
	int64 Instant = -1;
	int32 FiredAtInstant = 0;
	for (;;)
	{
		int32 Next = INDEX_NONE;
		for (int32 i = 0; i < Timers.Num(); ++i)
		{
			const FTimer& Timer = Timers[i];
			if (Timer.Due <= Target && (Next == INDEX_NONE || Timer.Due < Timers[Next].Due || (Timer.Due == Timers[Next].Due && Timer.Id < Timers[Next].Id)))
			{
				Next = i;
			}
		}
		if (Next == INDEX_NONE)
		{
			break;
		}
		FTimer Fired = MoveTemp(Timers[Next]);
		Timers.RemoveAt(Next);
		const int64 FireAt = FMath::Max(Nanos.load(std::memory_order_acquire), Fired.Due);
		FiredAtInstant = FireAt == Instant ? FiredAtInstant + 1 : 1;
		Instant = FireAt;
		checkf(FiredAtInstant <= MaxFiredAtOneInstant, TEXT("FVaRestVirtualClock::Advance: %d timers fired at %.9f s without time moving: a timer re-arms itself at the same instant"),
			FiredAtInstant, static_cast<double>(FireAt) / 1e9);
		Nanos.store(FireAt, std::memory_order_release);
		Fired.Callback();
	}
	Nanos.store(Target, std::memory_order_release);
}
