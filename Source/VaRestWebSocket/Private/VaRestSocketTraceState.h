// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "VaRestClock.h"

namespace VaRest::WebSocket
{
	/** Non-owning native view of Now(), whose interface permits reads on any thread. The GTK owner keeps the actual
	 * clock alive, retires this view after pipe quiescence and before dropping its strong clock reference on GTK.
	 * A read never holds an outbox/admission lock. The clock and its timer/callback storage never enter a worker owner. */
	class FSocketTraceClock
	{
	public:
		void InitializeOnGT(const IVaRestClock* InClock) { check(IsInGameThread()); FScopeLock Lock(&Mutex); Clock = InClock; }
		void RetireOnGT() { check(IsInGameThread()); FScopeLock Lock(&Mutex); Clock = nullptr; }
		double Now() const { FScopeLock Lock(&Mutex); return Clock ? Clock->Now() : 0.0; }
	private:
		mutable FCriticalSection Mutex;
		const IVaRestClock* Clock = nullptr;
	};
	struct FSocketFrameStamp
	{
		uint64 Id = 0;
		double CalledAt = 0.0;
	};
}
