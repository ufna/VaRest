// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketLink.h"

#include "HAL/PlatformTime.h"
#include "VaRestWebSocketLog.h"

VaRest::WebSocket::FSocketLink::FSocketLink(TFunction<void()> InOnEvents)
	: OnEvents(MoveTemp(InOnEvents))
{
}

void VaRest::WebSocket::FSocketLink::Push(FVaRestSocketEvent&& Event, bool bNotify)
{
	if (bTerminalQueued)
	{
		// Plan decision 2, made loud: a producer that queues after the end has a bug
		UE_LOG(LogVaRestSocket, Error, TEXT("an event (%s) after the connection's terminal event was dropped: a transport bug"), LexToString(Event.Type));
		return;
	}
	const bool bTerminal = Event.IsTerminal();
	bTerminalQueued = bTerminal;
	// Acquire, pairing with the handle's release: see bReleased in the header
	if (bReleased.load(std::memory_order_acquire))
	{
		// Discarded (plan decision 5), and not notified: nobody is left to pop it. The end still counts for HasEnded
		if (bTerminal)
		{
			bEnded.store(true, std::memory_order_release);
		}
		return;
	}
	if (Event.Type == EVaRestSocketEventType::Message)
	{
		if (Event.Bytes.Max() == Event.Bytes.Num())
		{
			// The slack byte FVaRestJson::ParseOwned wants (core 2.1)
			Event.Bytes.Reserve(Event.Bytes.Num() + 1);
		}
		// Before the enqueue, so the consumer's Consumed for this message always comes after it (see the header); relaxed: a count
		PendingInboundBytes.fetch_add(Event.Bytes.Num(), std::memory_order_relaxed);
	}
	Event.Time = FPlatformTime::Seconds();
	Queue.Enqueue(MoveTemp(Event));
	if (bTerminal)
	{
		// After the enqueue, with release: HasEnded() true (an acquire load) means the terminal event can be popped
		bEnded.store(true, std::memory_order_release);
	}
	if (bNotify)
	{
		// Last: nothing of this Push is left to do when OnEvents runs, so it may pop or push again
		Notify();
	}
}

void VaRest::WebSocket::FSocketLink::Notify() const
{
	// A released handle's events are discarded (plan decision 5), so nothing is left to tell it about: this also covers a
	// notification posted before the release that runs after it (the fake's deferred one, the transport's refusal task)
	if (OnEvents && !bReleased.load(std::memory_order_acquire))
	{
		OnEvents();
	}
}

int32 VaRest::WebSocket::FSocketLink::PopAll(TArray<FVaRestSocketEvent>& Out)
{
	int32 Popped = 0;
	FVaRestSocketEvent Event;
	while (Queue.Dequeue(Event))
	{
		Out.Add(MoveTemp(Event));
		++Popped;
	}
	return Popped;
}

void VaRest::WebSocket::FSocketLink::NoteBuffered(int64 HighWater)
{
	// Relaxed: the Send that added these bytes reached the producer through a queue (the service's commands, or the same
	// thread in the fake), so this load sees that add or a later value of the count
	if (BufferedBytes.load(std::memory_order_relaxed) >= HighWater)
	{
		bHighWaterReached = true;
	}
}

void VaRest::WebSocket::FSocketLink::ReleaseBuffered(int64 Bytes)
{
	// Relaxed: a read-modify-write reads the latest value in the count's modification order, so 0 here is a true 0
	const int64 Left = BufferedBytes.fetch_sub(Bytes, std::memory_order_relaxed) - Bytes;
	if (Left == 0 && bHighWaterReached && !bTerminalQueued)
	{
		bHighWaterReached = false;
		FVaRestSocketEvent Drained;
		Drained.Type = EVaRestSocketEventType::Drained;
		Push(MoveTemp(Drained));
	}
}
