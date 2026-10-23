// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Tasks/Task.h"
#include "VaRestSocketLink.h"

#include <atomic>

namespace VaRest::WebSocket::VaRestSocketLinkTestPrivate
{
	/** What a round's producer task, its OnEvents and the test share: captured by reference count, never from the stack */
	struct FShared
	{
		std::atomic<int32> Notified{ 0 };
		std::atomic<bool> bProducerOnGameThread{ true };
		/** The consumer popped message 0: the producer waits for it, so the rest is pushed while the consumer pops */
		std::atomic<bool> bFirstPopped{ false };
	};

	/** A Message carrying Index as its four bytes */
	FVaRestSocketEvent Numbered(int32 Index)
	{
		FVaRestSocketEvent Out;
		Out.Type = EVaRestSocketEventType::Message;
		Out.Bytes.SetNumUninitialized(sizeof(int32));
		FMemory::Memcpy(Out.Bytes.GetData(), &Index, sizeof(int32));
		return Out;
	}

	FVaRestSocketEvent Typed(EVaRestSocketEventType Type)
	{
		FVaRestSocketEvent Out;
		Out.Type = Type;
		return Out;
	}

	/** A Send's add, as the handles make it: returns the count that add produced, which NoteBuffered takes (review I-1) */
	int64 AddBuffered(FSocketLink& Target, int64 Bytes)
	{
		return Target.BufferedBytes.fetch_add(Bytes, std::memory_order_relaxed) + Bytes;
	}

	int32 NumberOf(const FVaRestSocketEvent& Event)
	{
		int32 Out = -1;
		if (Event.Bytes.Num() == sizeof(int32))
		{
			FMemory::Memcpy(&Out, Event.Bytes.GetData(), sizeof(int32));
		}
		return Out;
	}

	/**
	 * An OnEvents that pops inside the producer's Push, as a consumer on the producer's own thread may (the fake's game
	 * thread). Link points at the test's stack link, which owns this callback: the callback cannot outlive it
	 */
	struct FReentrant
	{
		FSocketLink* Link = nullptr;
		TArray<FVaRestSocketEvent> Popped;
		int32 Calls = 0;
		int64 PendingWhenMessagePopped = -1;
		bool bEndedWhenTerminalPopped = false;
	};

	/** What one round saw */
	struct FRoundResult
	{
		/** The Open came first */
		bool bOpened = false;
		/** Messages popped in order after it, 0, 1, 2, ... */
		int32 InOrder = 0;
		/** A message out of order, a second copy, or an event that is not a Message */
		bool bDisordered = false;
		bool bTerminal = false;
		bool bAfterTerminal = false;
		/** HasEnded was true, and the pop right after it did not reach the terminal event (bEnded published too early) */
		bool bEndedEarly = false;
		/** Pops that returned events */
		int32 Batches = 0;
		bool bJoined = false;
		bool bProducerOnGameThread = true;
		int32 Notified = 0;
		int64 PendingAfter = -1;
	};

	/**
	 * Game thread: a UE::Tasks producer pushes an Open, Count messages and a Closed into one link while this thread pops, as
	 * the service thread and the game thread do (WebSocket spec 2.1). After message 0 the producer waits until a pop returned
	 * events, so everything after it is pushed while this thread is popping. Both sides spin with a pause (the consumer yields every
	 * 4096 empty polls) under a deadline: no fixed sleep anywhere.
	 */
	FRoundResult RunRound(int32 Count)
	{
		const TSharedRef<FShared, ESPMode::ThreadSafe> Shared = MakeShared<FShared, ESPMode::ThreadSafe>();
		// OnEvents runs on the producer's thread (VaRestSocketTransport.h): an atomic count, nothing else
		const TSharedRef<FSocketLink, ESPMode::ThreadSafe> Link = MakeShared<FSocketLink, ESPMode::ThreadSafe>([Shared] { Shared->Notified.fetch_add(1, std::memory_order_relaxed); });
		const double Deadline = FPlatformTime::Seconds() + 30.0;
		const UE::Tasks::FTask Producer = UE::Tasks::Launch(TEXT("VaRest.Test.LinkProducer"), [Link, Shared, Count, Deadline]
		{
			Shared->bProducerOnGameThread.store(IsInGameThread(), std::memory_order_relaxed);
			Link->Push(Typed(EVaRestSocketEventType::Open));
			for (int32 Pushed = 0; Pushed < Count; ++Pushed)
			{
				Link->Push(Numbered(Pushed));
				while (Pushed == 0 && !Shared->bFirstPopped.load(std::memory_order_acquire) && FPlatformTime::Seconds() < Deadline)
				{
					FPlatformProcess::Yield();
				}
			}
			FVaRestSocketEvent Final;
			Final.Type = EVaRestSocketEventType::Closed;
			Final.End.Code = 1000;
			Link->Push(MoveTemp(Final));
		});

		FRoundResult Result;
		TArray<FVaRestSocketEvent> Batch;
		uint32 EmptyPolls = 0;
		while (!Result.bTerminal && FPlatformTime::Seconds() < Deadline)
		{
			// Read before the pop: true promises that this pop reaches the terminal event (bEnded, VaRestSocketLink.h)
			const bool bEndedBefore = Link->bEnded.load(std::memory_order_acquire);
			Batch.Reset();
			Link->PopAll(Batch);
			for (const FVaRestSocketEvent& Event : Batch)
			{
				if (Result.bTerminal)
				{
					Result.bAfterTerminal = true;
				}
				else if (Event.IsTerminal())
				{
					Result.bTerminal = true;
				}
				else if (Event.Type == EVaRestSocketEventType::Open && !Result.bOpened && Result.InOrder == 0)
				{
					Result.bOpened = true;
				}
				else if (Event.Type == EVaRestSocketEventType::Message && Result.bOpened && NumberOf(Event) == Result.InOrder)
				{
					++Result.InOrder;
					// The consumer's Consumed, as the handles do it
					Link->PendingInboundBytes.fetch_sub(Event.Bytes.Num(), std::memory_order_relaxed);
				}
				else
				{
					Result.bDisordered = true;
				}
			}
			if (bEndedBefore && !Result.bTerminal)
			{
				Result.bEndedEarly = true;
				break;
			}
			if (Batch.Num() > 0)
			{
				++Result.Batches;
				Shared->bFirstPopped.store(true, std::memory_order_release);
			}
			else
			{
				if ((++EmptyPolls & 4095) == 0)
				{
					FPlatformProcess::YieldThread();
				}
				else
				{
					FPlatformProcess::Yield();
				}
			}
		}
		// The join. Not Wait(): it may run a task that has not started, here (Task.h:56-57, :67-68). The producer is past its
		// last Push once the terminal event was popped; the body runs (TaskPrivate.h:524) before Close (:541) sets the seq_cst
		// flag IsCompleted reads (:781, :787), so the body's notifications are visible below
		while (!Producer.IsCompleted() && FPlatformTime::Seconds() < Deadline + 5.0)
		{
			FPlatformProcess::YieldThread();
		}
		Result.bJoined = Producer.IsCompleted();
		Result.bProducerOnGameThread = Shared->bProducerOnGameThread.load(std::memory_order_relaxed);
		Result.Notified = Shared->Notified.load(std::memory_order_relaxed);
		Result.PendingAfter = Link->PendingInboundBytes.load(std::memory_order_relaxed);
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketLinkStressTest, "VaRest.WebSocket.Link.Stress",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketLinkStressTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::VaRestSocketLinkTestPrivate;
	// One long round: every event arrives once, in order, while the two threads overlap
	constexpr int32 Many = 100000;
	const FRoundResult Long = RunRound(Many);
	if (!TestTrue(TEXT("the producer finished (joined)"), Long.bJoined))
	{
		return false;
	}
	TestFalse(TEXT("the producer ran off the game thread, so the round is concurrent"), Long.bProducerOnGameThread);
	TestTrue(TEXT("... and the pops overlapped the pushes (the producer waited for the first pop)"), Long.Batches >= 2);
	AddInfo(FString::Printf(TEXT("%d messages arrived in %d pops"), Many, Long.Batches));
	TestTrue(TEXT("the Open first"), Long.bOpened);
	TestEqual(TEXT("every message arrived once, in order"), Long.InOrder, Many);
	TestFalse(TEXT("... none out of order or twice"), Long.bDisordered);
	TestTrue(TEXT("... then the terminal event"), Long.bTerminal);
	TestFalse(TEXT("... and nothing after it"), Long.bAfterTerminal);
	TestFalse(TEXT("HasEnded never ran ahead of the terminal event"), Long.bEndedEarly);
	TestEqual(TEXT("one notification per event, on the producer's thread"), Long.Notified, Many + 2);
	TestEqual(TEXT("every message's bytes counted in and consumed out"), Long.PendingAfter, static_cast<int64>(0));

	// Many short rounds: HasEnded read while the terminal event is being queued, each round a fresh chance to see it early
	constexpr int32 Rounds = 2000;
	int32 Completed = 0;
	int32 EndedEarly = 0;
	int32 Wrong = 0;
	for (int32 Round = 0; Round < Rounds; ++Round)
	{
		const FRoundResult Short = RunRound(1);
		if (!Short.bJoined)
		{
			break;
		}
		++Completed;
		EndedEarly += Short.bEndedEarly ? 1 : 0;
		Wrong += (!Short.bOpened || Short.InOrder != 1 || Short.bDisordered || !Short.bTerminal || Short.bAfterTerminal || Short.Notified != 3 || Short.PendingAfter != 0) ? 1 : 0;
	}
	TestEqual(TEXT("every short round's producer finished"), Completed, Rounds);
	TestEqual(TEXT("no short round saw HasEnded before its terminal event could be popped"), EndedEarly, 0);
	TestEqual(TEXT("every short round: its open, its message, its end, three notifications, nothing pending"), Wrong, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketLinkReentrantTest, "VaRest.WebSocket.Link.ReentrantNotify",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketLinkReentrantTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	using namespace VaRest::WebSocket::VaRestSocketLinkTestPrivate;
	const TSharedRef<FReentrant> Inside = MakeShared<FReentrant>();
	FSocketLink Link([Inside]
	{
		++Inside->Calls;
		const int32 Before = Inside->Popped.Num();
		Inside->Link->PopAll(Inside->Popped);
		for (int32 Index = Before; Index < Inside->Popped.Num(); ++Index)
		{
			if (Inside->Popped[Index].Type == EVaRestSocketEventType::Message)
			{
				Inside->PendingWhenMessagePopped = Inside->Link->PendingInboundBytes.load(std::memory_order_relaxed);
			}
			else if (Inside->Popped[Index].IsTerminal())
			{
				Inside->bEndedWhenTerminalPopped = Inside->Link->bEnded.load(std::memory_order_acquire);
			}
		}
	});
	Inside->Link = &Link;
	Link.Push(Typed(EVaRestSocketEventType::Open));
	Link.Push(Numbered(7));
	FVaRestSocketEvent Final;
	Final.Type = EVaRestSocketEventType::Closed;
	Final.End.Code = 1000;
	Link.Push(MoveTemp(Final));
	TestEqual(TEXT("OnEvents ran once per Push"), Inside->Calls, 3);
	TestTrue(TEXT("... and popped every event inside the Pushes, in order"), Inside->Popped.Num() == 3 && Inside->Popped[0].Type == EVaRestSocketEventType::Open
		&& Inside->Popped[1].Type == EVaRestSocketEventType::Message && NumberOf(Inside->Popped[1]) == 7 && Inside->Popped[2].IsTerminal());
	// The notification is Push's last step: what it pops inside is fully accounted for
	TestEqual(TEXT("the message popped inside its Push was already counted"), Inside->PendingWhenMessagePopped, static_cast<int64>(4));
	TestTrue(TEXT("the terminal event popped inside its Push had already ended the link"), Inside->bEndedWhenTerminalPopped);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketLinkDrainedTest, "VaRest.WebSocket.Link.Drained",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketLinkDrainedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	using namespace VaRest::WebSocket::VaRestSocketLinkTestPrivate;
	FSocketLink Link(nullptr);
	TArray<FVaRestSocketEvent> Events;
	Link.Push(Typed(EVaRestSocketEventType::Open));
	TestEqual(TEXT("the open"), Link.PopAll(Events), 1);
	Events.Reset();
	// Spec 2.4: a return to zero is Drained only once the high-water mark was reached since the last one
	Link.NoteBuffered(AddBuffered(Link, 5), 8);
	Link.ReleaseBuffered(5);
	TestEqual(TEXT("below the high-water mark: no Drained"), Link.PopAll(Events), 0);
	Link.NoteBuffered(AddBuffered(Link, 8), 8);
	Link.ReleaseBuffered(3);
	TestEqual(TEXT("at the mark, not yet empty: no Drained"), Link.PopAll(Events), 0);
	Link.ReleaseBuffered(5);
	TestTrue(TEXT("... empty: Drained"), Link.PopAll(Events) == 1 && Events.Num() == 1 && Events[0].Type == EVaRestSocketEventType::Drained);
	Link.NoteBuffered(AddBuffered(Link, 2), 8);
	Link.ReleaseBuffered(2);
	TestEqual(TEXT("the mark is spent by the Drained: a small send after it drains quietly"), Link.PopAll(Events), 0);
	// Review I-1: the real transport's producer notes a send after the service thread may already have released bytes of
	// earlier frames. Frame A (200) is buffered, frame B (100) brings the count to 300 -- past the mark, as the app saw it --
	// one 64-byte fragment of A leaves before B's note runs, and the rest empties the buffer: that is a Drained
	Link.NoteBuffered(AddBuffered(Link, 200), 256);
	const int64 AfterB = AddBuffered(Link, 100);
	Link.ReleaseBuffered(64);
	Link.NoteBuffered(AfterB, 256);
	Link.ReleaseBuffered(236);
	Events.Reset();
	TestTrue(TEXT("a crossing seen by the sender is Drained, even when bytes left before the producer noted it"), Link.PopAll(Events) == 1 && Events.Num() == 1
		&& Events[0].Type == EVaRestSocketEventType::Drained);
	// Nothing after the terminal event, Drained included -- and without an Error line: ReleaseBuffered checks first
	Link.NoteBuffered(AddBuffered(Link, 9), 8);
	FVaRestSocketEvent Final;
	Final.Type = EVaRestSocketEventType::Closed;
	Final.End.Code = 1006;
	Link.Push(MoveTemp(Final));
	Link.ReleaseBuffered(9);
	Events.Reset();
	TestTrue(TEXT("after the end, the return to zero queues nothing"), Link.PopAll(Events) == 1 && Events.Num() == 1 && Events[0].Type == EVaRestSocketEventType::Closed);
	TestTrue(TEXT("... and the link has ended"), Link.bEnded.load(std::memory_order_acquire));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketLinkLifecycleTest, "VaRest.WebSocket.Link.Lifecycle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketLinkLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	using namespace VaRest::WebSocket::VaRestSocketLinkTestPrivate;
	// Review M-2: plan decision 2's types -- at most one Open, Error only before it, Closed only after it. A push that breaks
	// them is a transport bug (or a mis-scripted fake): logged, naming the connection (review M-1), and still queued
	AddExpectedErrorPlain(TEXT("lifecycle#1: a second Open was queued"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("lifecycle#2: Closed was queued before any Open"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("lifecycle#3: Error was queued after the Open"), EAutomationExpectedErrorFlags::Contains, 1);
	TArray<FVaRestSocketEvent> Events;
	FSocketLink Twice(nullptr, TEXT("lifecycle#1"));
	Twice.Push(Typed(EVaRestSocketEventType::Open));
	Twice.Push(Typed(EVaRestSocketEventType::Open));
	Twice.Push(Typed(EVaRestSocketEventType::Closed));
	TestEqual(TEXT("a second Open is logged and still queued, and so is the end"), Twice.PopAll(Events), 3);
	FSocketLink Unopened(nullptr, TEXT("lifecycle#2"));
	Unopened.Push(Typed(EVaRestSocketEventType::Closed));
	Events.Reset();
	TestTrue(TEXT("a Closed with no Open is logged, and the end still reaches the consumer"), Unopened.PopAll(Events) == 1 && Unopened.bEnded.load(std::memory_order_acquire));
	FSocketLink Opened(nullptr, TEXT("lifecycle#3"));
	Opened.Push(Typed(EVaRestSocketEventType::Open));
	Opened.Push(Typed(EVaRestSocketEventType::Error));
	Events.Reset();
	TestTrue(TEXT("an Error after the Open is logged, and the end still reaches the consumer"), Opened.PopAll(Events) == 2 && Opened.bEnded.load(std::memory_order_acquire));
	// The shapes the transport makes log nothing (any line here would be an unexpected Error)
	FSocketLink Refused(nullptr, TEXT("lifecycle#4"));
	Refused.Push(Typed(EVaRestSocketEventType::Error));
	FSocketLink Normal(nullptr, TEXT("lifecycle#5"));
	Normal.Push(Typed(EVaRestSocketEventType::Open));
	Normal.Push(Numbered(1));
	Normal.Push(Typed(EVaRestSocketEventType::Closed));
	Events.Reset();
	TestTrue(TEXT("Error before any Open, and Open, Message, Closed, are quiet"), Refused.PopAll(Events) == 1 && Normal.PopAll(Events) == 3);
	return true;
}

#endif
