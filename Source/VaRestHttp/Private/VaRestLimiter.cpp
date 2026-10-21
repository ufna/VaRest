// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLimiter.h"

#include "VaRestHttpLog.h"

static_assert(static_cast<int32>(EVaRestPriority::Highest) == 0 && static_cast<int32>(EVaRestPriority::Low) == 3, "FLimiter indexes its queues by priority");

int32 VaRest::Http::FLimiter::LevelOf(EVaRestPriority Priority)
{
	const int32 Level = static_cast<int32>(Priority);
	return Level >= 0 && Level < NumLevels ? Level : NumLevels - 1;
}

void VaRest::Http::FLimiter::SetCapacity(int32 InCapacity)
{
	check(IsInGameThread());
	Capacity = FMath::Max(1, InCapacity);
}

void VaRest::Http::FLimiter::Acquire(uint64 RequestId, EVaRestPriority Priority, FGrant&& OnGrant, FAbort&& OnAbort)
{
	check(IsInGameThread());
	const int32 Level = LevelOf(Priority);
	if (Level != static_cast<int32>(Priority))
	{
		// EVaRestPriority is a uint8 BlueprintType enum: a cast or a Blueprint byte can carry any value, and indexing with
		// it would write past Queues
		UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu VaRest: priority %d is not an EVaRestPriority; the request queues as Low"),
			RequestId, static_cast<int32>(Priority));
	}
	if (Active < Capacity && NumQueued() == 0)
	{
		++Active;
		OnGrant();
		return;
	}
	FEntry& Entry = Queues[Level].AddDefaulted_GetRef();
	Entry.RequestId = RequestId;
	Entry.Grant = MoveTemp(OnGrant);
	Entry.OnAbort = MoveTemp(OnAbort);
}

void VaRest::Http::FLimiter::Release()
{
	check(IsInGameThread());
	// Review Minor 1: a Release without a grant (a double release) over-grants from then on whatever this counter does, so
	// it must not pass quietly. The ensure fails the test that does it; where ensures are compiled out the count still
	// never goes negative, which would add one more slot.
	if (ensureMsgf(Active > 0, TEXT("FLimiter: Release() without a matching grant (a double release)")))
	{
		--Active;
	}
	// Review M-1: inside a grant (an exchange canceled at dispatch answers at once) only the slot is freed; the pump
	// already running takes the next entry. Recursing instead grows the stack with every dead entry in the queue.
	if (!bPumping)
	{
		Pump();
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

void VaRest::Http::FLimiter::Pump()
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
		while (Level < NumLevels && Queues[Level].Num() == 0)
		{
			++Level;
		}
		if (Level == NumLevels)
		{
			return;
		}
		FEntry Next = MoveTemp(Queues[Level][0]);
		Queues[Level].RemoveAt(0);
		++Active;
		Next.Grant();
	}
}
