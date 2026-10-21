// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLimiter.h"

static_assert(static_cast<int32>(EVaRestPriority::Highest) == 0 && static_cast<int32>(EVaRestPriority::Low) == 3, "FLimiter indexes its queues by priority");

void VaRest::Http::FLimiter::SetCapacity(int32 InCapacity)
{
	check(IsInGameThread());
	Capacity = FMath::Max(1, InCapacity);
}

void VaRest::Http::FLimiter::Acquire(uint64 RequestId, EVaRestPriority Priority, double Now, FGrant&& OnGrant, FAbort&& OnAbort)
{
	check(IsInGameThread());
	if (Active < Capacity && NumQueued() == 0)
	{
		++Active;
		OnGrant(0.0);
		return;
	}
	FEntry& Entry = Queues[static_cast<int32>(Priority)].AddDefaulted_GetRef();
	Entry.RequestId = RequestId;
	Entry.EnqueuedAt = Now;
	Entry.Grant = MoveTemp(OnGrant);
	Entry.OnAbort = MoveTemp(OnAbort);
}

void VaRest::Http::FLimiter::Release(double Now)
{
	check(IsInGameThread());
	Active = FMath::Max(0, Active - 1);
	// Review M-1: inside a grant (an exchange canceled at dispatch answers at once) only the slot is freed; the pump
	// already running takes the next entry. Recursing instead grows the stack with every dead entry in the queue.
	if (!bPumping)
	{
		Pump(Now);
	}
}

bool VaRest::Http::FLimiter::Abort(uint64 RequestId)
{
	check(IsInGameThread());
	for (TArray<FEntry>& Queue : Queues)
	{
		const int32 Index = Queue.IndexOfByPredicate([RequestId](const FEntry& Entry) { return Entry.RequestId == RequestId; });
		if (Index != INDEX_NONE)
		{
			FEntry Removed = MoveTemp(Queue[Index]);
			Queue.RemoveAt(Index);
			Removed.OnAbort();
			return true;
		}
	}
	return false;
}

int32 VaRest::Http::FLimiter::NumQueued() const
{
	int32 Count = 0;
	for (const TArray<FEntry>& Queue : Queues)
	{
		Count += Queue.Num();
	}
	return Count;
}

void VaRest::Http::FLimiter::Pump(double Now)
{
	check(IsInGameThread());
	if (bPumping)
	{
		return;
	}
	// A grant may finish at once (a canceled exchange) and Release; that Release only frees the slot (review M-1). Every
	// pass re-reads the queues and holds nothing in them across the call.
	TGuardValue<bool> Pumping(bPumping, true);
	while (Active < Capacity)
	{
		int32 Level = 0;
		while (Level < 4 && Queues[Level].Num() == 0)
		{
			++Level;
		}
		if (Level == 4)
		{
			return;
		}
		FEntry Next = MoveTemp(Queues[Level][0]);
		Queues[Level].RemoveAt(0);
		++Active;
		Next.Grant(Now - Next.EnqueuedAt);
	}
}
