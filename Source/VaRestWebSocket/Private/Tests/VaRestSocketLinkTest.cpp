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
		/** Messages popped in order, 0, 1, 2, ... */
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
	 * Game thread: a UE::Tasks producer pushes Count messages and a Closed into one link while this thread pops, as the
	 * service thread and the game thread do (WebSocket spec 2.1). After message 0 the producer waits until it was popped, so
	 * everything after it is pushed while this thread is popping. Both sides spin with a pause (the consumer yields every
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
				else if (Event.Type == EVaRestSocketEventType::Message && NumberOf(Event) == Result.InOrder)
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
	TestEqual(TEXT("every message arrived once, in order"), Long.InOrder, Many);
	TestFalse(TEXT("... none out of order or twice"), Long.bDisordered);
	TestTrue(TEXT("... then the terminal event"), Long.bTerminal);
	TestFalse(TEXT("... and nothing after it"), Long.bAfterTerminal);
	TestFalse(TEXT("HasEnded never ran ahead of the terminal event"), Long.bEndedEarly);
	TestEqual(TEXT("one notification per event, on the producer's thread"), Long.Notified, Many + 1);
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
		Wrong += (Short.InOrder != 1 || Short.bDisordered || !Short.bTerminal || Short.bAfterTerminal || Short.Notified != 2 || Short.PendingAfter != 0) ? 1 : 0;
	}
	TestEqual(TEXT("every short round's producer finished"), Completed, Rounds);
	TestEqual(TEXT("no short round saw HasEnded before its terminal event could be popped"), EndedEarly, 0);
	TestEqual(TEXT("every short round: its message, its end, two notifications, nothing pending"), Wrong, 0);
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
	Link.Push(Numbered(7));
	FVaRestSocketEvent Final;
	Final.Type = EVaRestSocketEventType::Closed;
	Final.End.Code = 1000;
	Link.Push(MoveTemp(Final));
	TestEqual(TEXT("OnEvents ran once per Push"), Inside->Calls, 2);
	TestTrue(TEXT("... and popped both events inside the Pushes, in order"), Inside->Popped.Num() == 2 && Inside->Popped[0].Type == EVaRestSocketEventType::Message
		&& NumberOf(Inside->Popped[0]) == 7 && Inside->Popped[1].IsTerminal());
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
	FSocketLink Link(nullptr);
	TArray<FVaRestSocketEvent> Events;
	// Spec 2.4: a return to zero is Drained only once the high-water mark was reached since the last one
	Link.BufferedBytes.fetch_add(5, std::memory_order_relaxed);
	Link.NoteBuffered(8);
	Link.ReleaseBuffered(5);
	TestEqual(TEXT("below the high-water mark: no Drained"), Link.PopAll(Events), 0);
	Link.BufferedBytes.fetch_add(8, std::memory_order_relaxed);
	Link.NoteBuffered(8);
	Link.ReleaseBuffered(3);
	TestEqual(TEXT("at the mark, not yet empty: no Drained"), Link.PopAll(Events), 0);
	Link.ReleaseBuffered(5);
	TestTrue(TEXT("... empty: Drained"), Link.PopAll(Events) == 1 && Events.Num() == 1 && Events[0].Type == EVaRestSocketEventType::Drained);
	Link.BufferedBytes.fetch_add(2, std::memory_order_relaxed);
	Link.NoteBuffered(8);
	Link.ReleaseBuffered(2);
	TestEqual(TEXT("the mark is spent by the Drained: a small send after it drains quietly"), Link.PopAll(Events), 0);
	// Nothing after the terminal event, Drained included -- and without an Error line: ReleaseBuffered checks first
	Link.BufferedBytes.fetch_add(9, std::memory_order_relaxed);
	Link.NoteBuffered(8);
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

#endif
