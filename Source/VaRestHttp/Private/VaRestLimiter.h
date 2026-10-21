// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestHttpTypes.h"

namespace VaRest::Http
{
	/**
	 * Spec 4.6: MaxConcurrent slots and a priority queue, Highest first, FIFO within a priority. Strict priority, as the
	 * spec says (review M-19); the waits a user can see are documented on FVaRestClient::WithMaxConcurrent and
	 * TVaRestRequest::Priority. Game thread.
	 *
	 * It keeps no time (review Minor 3): the Release that grants an entry belongs to another exchange, whose configuration
	 * snapshot may hold another clock (WithClock()), so a wait is measured by the waiting caller, on its own clock.
	 */
	class FLimiter
	{
	public:
		using FGrant = TUniqueFunction<void()>;
		using FAbort = TUniqueFunction<void()>;

		/**
		 * At least 1. Grants nothing by itself: the caller pumps, from a game-thread task (review M-2). Below NumActive() it
		 * takes nothing back: no grant until enough granted attempts have ended.
		 */
		void SetCapacity(int32 InCapacity);
		/** Grants queued attempts while slots are free. Not re-entrant: a Release inside a grant only frees its slot (review M-1). */
		void Pump();
		/**
		 * OnGrant now when a slot is free and nothing waits, else in its turn; OnAbort instead if Abort() takes it out of the
		 * queue. A value outside EVaRestPriority queues as Low.
		 */
		void Acquire(uint64 RequestId, EVaRestPriority Priority, FGrant&& OnGrant, FAbort&& OnAbort);
		/** A granted attempt ended: exactly once per grant. A Release without one is an ensureAlways, every time (review Minor 1, P12.1). */
		void Release();
		/** Takes a queued attempt out and runs its OnAbort; false when it is not queued */
		bool Abort(uint64 RequestId);

		int32 NumActive() const { return Active; }
		int32 NumQueued() const;
		int32 GetCapacity() const { return Capacity; }

	private:
		static constexpr int32 NumLevels = 4;

		/** The queue a priority uses; a value outside the enum (a cast, a Blueprint byte) maps to Low (review Minor 2) */
		static int32 LevelOf(EVaRestPriority Priority);

		struct FEntry
		{
			uint64 RequestId = 0;
			FGrant Grant;
			FAbort OnAbort;
		};

		/**
		 * Indexed by LevelOf: Highest = 0 ... Low = 3. Plain arrays: a grant's RemoveAt(0) and Abort's scan are linear in a
		 * level's length, so CancelAll over N queued is O(N^2) -- fine for the tens to hundreds a client queues; a head
		 * index per level (or TDeque with tombstones for Abort) if M4's offline queue ever feeds thousands (review Minor 8).
		 */
		TArray<FEntry> Queues[NumLevels];
		int32 Active = 0;
		int32 Capacity = 16;
		bool bPumping = false;
	};
}
