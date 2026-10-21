// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestHttpTypes.h"

namespace VaRest::Http
{
	/**
	 * Spec 4.6: MaxConcurrent slots and a priority queue, Highest first, FIFO within a priority. Strict priority, as the
	 * spec says: under sustained higher-priority load a Low request can wait indefinitely (review M-19). Game thread.
	 */
	class FLimiter
	{
	public:
		using FGrant = TUniqueFunction<void(double /*WaitedSeconds*/)>;
		using FAbort = TUniqueFunction<void()>;

		/** At least 1. Grants nothing by itself: the caller pumps, from a game-thread task (review M-2) */
		void SetCapacity(int32 InCapacity);
		/** Grants queued attempts while slots are free. Not re-entrant: a Release inside a grant only frees its slot (review M-1). */
		void Pump(double Now);
		/** OnGrant now when a slot is free, else when one is; OnAbort instead if Abort() takes it out of the queue */
		void Acquire(uint64 RequestId, EVaRestPriority Priority, double Now, FGrant&& OnGrant, FAbort&& OnAbort);
		/** A granted attempt ended */
		void Release(double Now);
		/** Takes a queued attempt out and runs its OnAbort; false when it is not queued */
		bool Abort(uint64 RequestId);

		int32 NumActive() const { return Active; }
		int32 NumQueued() const;
		int32 GetCapacity() const { return Capacity; }

	private:
		struct FEntry
		{
			uint64 RequestId = 0;
			double EnqueuedAt = 0.0;
			FGrant Grant;
			FAbort OnAbort;
		};

		/** Indexed by EVaRestPriority: Highest = 0 ... Low = 3 */
		TArray<FEntry> Queues[4];
		int32 Active = 0;
		int32 Capacity = 16;
		bool bPumping = false;
	};
}
