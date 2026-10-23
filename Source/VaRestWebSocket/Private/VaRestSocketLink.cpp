// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketLink.h"

#include "HAL/PlatformTime.h"
#include "VaRestWebSocketLog.h"

VaRest::WebSocket::FSocketLink::FSocketLink(TFunction<void()> InOnEvents, FString InLabel)
	: OnEvents(MoveTemp(InOnEvents))
	, Label(InLabel.IsEmpty() ? FString(TEXT("ws#?")) : MoveTemp(InLabel))
{
}

void VaRest::WebSocket::FSocketLink::Push(FVaRestSocketEvent&& Event, bool bNotify)
{
	if (bTerminalQueued)
	{
		// Plan decision 2, made loud: a producer that queues after the end has a bug
		UE_LOG(LogVaRestSocket, Error, TEXT("%s: an event (%s) after the connection's terminal event was dropped: a transport bug"), *Label, LexToString(Event.Type));
		return;
	}
	// Plan decision 2's types, made loud (review M-2): logged, not dropped, so the end still reaches the consumer. Before the
	// released check, so a mis-scripted fake is caught whether or not its handle is still held
	if (Event.Type == EVaRestSocketEventType::Open && bOpenQueued)
	{
		UE_LOG(LogVaRestSocket, Error, TEXT("%s: a second Open was queued: a transport bug"), *Label);
	}
	else if (Event.Type == EVaRestSocketEventType::Closed && !bOpenQueued)
	{
		UE_LOG(LogVaRestSocket, Error, TEXT("%s: Closed was queued before any Open (an attempt that never opened ends with Error): a transport bug"), *Label);
	}
	else if (Event.Type == EVaRestSocketEventType::Error && bOpenQueued)
	{
		UE_LOG(LogVaRestSocket, Error, TEXT("%s: Error was queued after the Open (an open connection ends with Closed): a transport bug"), *Label);
	}
	else if (!bOpenQueued && (Event.Type == EVaRestSocketEventType::Message || Event.Type == EVaRestSocketEventType::Pong || Event.Type == EVaRestSocketEventType::Drained))
	{
		// Task 3 residual: nothing comes ahead of the Open -- a send before it is written, and so Drained, only after it (plan decision 18)
		UE_LOG(LogVaRestSocket, Error, TEXT("%s: %s was queued before the Open (nothing comes ahead of it): a transport bug"), *Label, LexToString(Event.Type));
	}
	bOpenQueued = bOpenQueued || Event.Type == EVaRestSocketEventType::Open;
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
	// notification posted before the release that runs after it (the fake's deferred one, the transport's refusal task).
	// Best effort across threads: a release between this check and the call still gets the call (see the header)
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

void VaRest::WebSocket::FSocketLink::NoteBuffered(int64 BufferedAfterSend, int64 HighWater)
{
	// The value the Send's own add produced, never a re-read of the count: a re-read can be lower already (review I-1, header)
	if (BufferedAfterSend >= HighWater)
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

VaRest::WebSocket::FOutboundClose VaRest::WebSocket::MakeOutboundClose(const FString& Label, int32 Code, FStringView Reason)
{
	FOutboundClose Out;
	bool bReplaced = false;
	bool bTruncated = false;
	Out.Code = SanitizeCloseCode(Code, bReplaced);
	Out.Reason = CloseReasonBytes(Reason, bTruncated);
	if (bReplaced)
	{
		UE_LOG(LogVaRestSocket, Warning, TEXT("%s: close code %d cannot be sent (RFC 6455 7.4); 1000 is sent instead"), *Label, Code);
	}
	if (bTruncated)
	{
		UE_LOG(LogVaRestSocket, Warning, TEXT("%s: the close reason is cut to %d bytes (RFC 6455 5.5)"), *Label, MaxCloseReasonBytes);
	}
	return Out;
}
