// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Templates/Function.h"

#include <atomic>

/** A timer's id; 0 = none */
struct FVaRestTimerHandle
{
	uint64 Id = 0;

	bool IsValid() const { return Id != 0; }
	void Invalidate() { Id = 0; }
};

namespace VaRest::Http
{
	/**
	 * Any thread: a timer id unique in the process (review M-7). Per-clock counters would let a handle kept across a
	 * WithClock cancel an unrelated timer on the new clock.
	 */
	VARESTHTTP_API uint64 NewTimerId();
}

/**
 * Time and timers for the HTTP core, injectable so tests run on virtual time (spec 4.1). M3 times progress, the fake's
 * delays and timeouts, and trace events with it; M4's retry and backoff use the same clock.
 */
class IVaRestClock
{
public:
	virtual ~IVaRestClock() = default;

	/** Seconds, monotonic. Any thread: the HTTP thread stamps transport times with it (Inspector IN-3). */
	virtual double Now() const = 0;

	/** Game thread. Runs Callback once on the game thread after DelaySeconds; 0 or less: on the next tick. */
	virtual FVaRestTimerHandle SetTimer(double DelaySeconds, TUniqueFunction<void()>&& Callback) = 0;

	/** Game thread. Cancels a pending timer and invalidates Handle; a fired or invalid handle is a no-op. */
	virtual void ClearTimer(FVaRestTimerHandle& Handle) = 0;
};

/**
 * FPlatformTime::Seconds and one-shot tickers on the core ticker (Ticker.h:35, :56, :66), which the engine ticks on the game
 * thread. Now() is wall time, but the ticker counts the deltas it is given: the engine passes FApp::GetDeltaTime()
 * (LaunchEngineLoop.cpp:6103), so under a fixed timestep a timer's delay is measured in game time and can drift from Now().
 * Delays are clamped exactly like FVaRestVirtualClock::ToNanos: negative is 0, NaN and anything above MaxSeconds is MaxSeconds.
 */
class VARESTHTTP_API FVaRestRealClock final : public IVaRestClock, public TSharedFromThis<FVaRestRealClock, ESPMode::ThreadSafe>
{
public:
	/** The process-wide instance clients use by default */
	static TSharedRef<FVaRestRealClock, ESPMode::ThreadSafe> Get();

	virtual double Now() const override;
	virtual FVaRestTimerHandle SetTimer(double DelaySeconds, TUniqueFunction<void()>&& Callback) override;
	virtual void ClearTimer(FVaRestTimerHandle& Handle) override;

private:
	/** Game thread */
	TMap<uint64, FTSTicker::FDelegateHandle> Pending;
};

/**
 * Virtual time for tests (spec 7.1): nothing fires until Advance. Time is kept in integer nanoseconds (review I1): in
 * doubles a tick re-armed at Now() + 0.1 falls due at 0.30000000000000004 while 0.1 + 0.1 + 0.05 + 0.05 reaches only 0.3,
 * so a timer due "now" would not fire. M4's backoff tests need the same determinism.
 */
class VARESTHTTP_API FVaRestVirtualClock final : public IVaRestClock
{
public:
	virtual double Now() const override { return static_cast<double>(Nanos.load(std::memory_order_acquire)) / 1e9; }
	virtual FVaRestTimerHandle SetTimer(double DelaySeconds, TUniqueFunction<void()>&& Callback) override;
	virtual void ClearTimer(FVaRestTimerHandle& Handle) override;

	/**
	 * Game thread. Moves time forward by DeltaSeconds, firing each due timer at its due time: in due order, ties in creation
	 * order, including timers that firing ones set when they fall due before the target. Advance(0) fires what is due now.
	 */
	void Advance(double DeltaSeconds);

	int32 NumPendingTimers() const { return Timers.Num(); }

	/**
	 * The longest delay, about 31.7 years. int64 nanoseconds overflow past 9.2e9 s, and RoundToInt64 doubles its input
	 * (UnrealPlatformMathSSE.h:118-121), so the conversion itself overflows past about 4.6e9 s (review N-7)
	 */
	static constexpr double MaxSeconds = 1e9;
	/** A fail-fast bound: this many timers fired at one instant is a timer re-arming itself at a zero delay (review N-2) */
	static constexpr int32 MaxFiredAtOneInstant = 100000;
	/** A fail-fast bound on one Advance: a timer re-arming itself at a small period over a huge Advance would run ~1e9 times silently (review M-2) */
	static constexpr int32 MaxFiredPerAdvance = 10000000;

	/** Seconds to whole nanoseconds; negative is 0, above MaxSeconds is MaxSeconds (clamped before scaling) */
	static int64 ToNanos(double InSeconds) { return FMath::RoundToInt64(FMath::Clamp(InSeconds, 0.0, MaxSeconds) * 1e9); }

private:
	struct FTimer
	{
		uint64 Id = 0;
		int64 Due = 0;
		TUniqueFunction<void()> Callback;
	};

	/** now + delay, saturating at INT64_MAX: past it a "never" timer would wrap negative and fire at once (review M-2) */
	static int64 SaturatedAdd(int64 A, int64 B) { return A > TNumericLimits<int64>::Max() - B ? TNumericLimits<int64>::Max() : A + B; }

	std::atomic<int64> Nanos{ 0 };
	TArray<FTimer> Timers;
	/** Advance is not re-entrant: a nested one with a later target would move Now() backwards (review M-1) */
	bool bAdvancing = false;
};
