// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include <limits>
#include "Tasks/Task.h"
#include "VaRestClock.h"
#include "VaRestHttpLog.h"
#include "VaRestHttpTesting.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpVirtualClockTest, "VaRest.Http.Clock.Virtual",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpVirtualClockTest::RunTest(const FString& Parameters)
{
	TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
	TArray<FString> Fired;
	Clock->SetTimer(2.0, [&Fired] { Fired.Add(TEXT("A")); });
	Clock->SetTimer(1.0, [&Fired] { Fired.Add(TEXT("B")); });
	Clock->SetTimer(1.0, [&Fired, &Clock] {
		Fired.Add(TEXT("C"));
		Clock->SetTimer(0.0, [&Fired] { Fired.Add(TEXT("C+0")); }); // due now: fires in this same Advance
	});
	FVaRestTimerHandle Dropped = Clock->SetTimer(1.5, [&Fired] { Fired.Add(TEXT("dropped")); });
	TestEqual(TEXT("nothing fires before Advance"), Fired.Num(), 0);
	Clock->Advance(0.5);
	TestEqual(TEXT("nothing is due at 0.5"), Fired.Num(), 0);
	Clock->ClearTimer(Dropped);
	TestFalse(TEXT("ClearTimer invalidates the handle"), Dropped.IsValid());
	Clock->Advance(0.5);
	TestEqualSensitive(TEXT("due order, ties in creation order, timers set while firing included"), FString::Join(Fired, TEXT(",")), FString(TEXT("B,C,C+0")));
	TestEqual(TEXT("Now is the target time"), Clock->Now(), 1.0);
	Clock->Advance(5.0);
	TestEqualSensitive(TEXT("a cleared timer never fires"), FString::Join(Fired, TEXT(",")), FString(TEXT("B,C,C+0,A")));
	TestEqual(TEXT("Now"), Clock->Now(), 6.0);
	TestEqual(TEXT("nothing pending"), Clock->NumPendingTimers(), 0);

	// Review I1: a tick re-armed at Now() + 0.1 must fall due exactly when 0.1 + 0.1 + 0.05 + 0.05 is reached
	TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Ticking = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
	int32 Ticks = 0;
	TFunction<void()> Arm;
	Arm = [&Arm, &Ticks, &Ticking] { ++Ticks; Ticking->SetTimer(0.1, [&Arm] { Arm(); }); };
	Ticking->SetTimer(0.1, [&Arm] { Arm(); });
	Ticking->Advance(0.1);
	Ticking->Advance(0.1);
	Ticking->Advance(0.05);
	Ticking->Advance(0.05);
	TestEqual(TEXT("three ticks by 0.3: integer nanoseconds, no drift"), Ticks, 3);
	TestEqual(TEXT("... and Now is 0.3 as written"), Ticking->Now(), 0.3);

	// Review N-7: RoundToInt64(x * 1e9) overflows past 9.2e9 s; an overflowed delay would be due at once
	TestEqual(TEXT("a huge delay is clamped before scaling"), FVaRestVirtualClock::ToNanos(TNumericLimits<double>::Max()), static_cast<int64>(1000000000000000000));
	TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Far = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
	bool bFarFired = false;
	Far->SetTimer(TNumericLimits<double>::Max(), [&bFarFired] { bFarFired = true; });
	Far->Advance(1.0);
	TestFalse(TEXT("... so a timer set for 'never' does not fire"), bFarFired);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRealClockTest, "VaRest.Http.Clock.Real",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRealClockTest::RunTest(const FString& Parameters)
{
	TSharedRef<FVaRestRealClock, ESPMode::ThreadSafe> Clock = FVaRestRealClock::Get();
	TSharedRef<bool, ESPMode::ThreadSafe> bFired = MakeShared<bool, ESPMode::ThreadSafe>(false);
	TSharedRef<bool, ESPMode::ThreadSafe> bCleared = MakeShared<bool, ESPMode::ThreadSafe>(false);
	const double Start = Clock->Now();
	Clock->SetTimer(0.05, [bFired] { *bFired = IsInGameThread(); });
	FVaRestTimerHandle Never = Clock->SetTimer(0.05, [bCleared] { *bCleared = true; });
	Clock->ClearTimer(Never);
	TestTrue(TEXT("the real clock's timer fires on the game thread, from the core ticker"),
		VaRest::Http::Testing::PumpUntil([bFired] { return *bFired; }, 2.0, /*bTickCoreTicker*/ true));
	TestTrue(TEXT("... after its delay"), Clock->Now() - Start >= 0.05);
	VaRest::Http::Testing::PumpUntil([] { return false; }, 0.1, true);
	TestFalse(TEXT("a cleared timer does not fire"), *bCleared);
	return true;
}

namespace VaRest::Http::VaRestHttpClockTestPrivate
{
	/** Callback-visible test state: shared, never stack references (a timer outlives a failing test) */
	struct FLog
	{
		TArray<FString> Fired;
		FVaRestTimerHandle Handle;

		FString Joined() const { return FString::Join(Fired, TEXT(",")); }
	};
	using FLogRef = TSharedRef<FLog, ESPMode::ThreadSafe>;
}

// Review I-1: Advance re-scans after every callback, so a timer cleared by a callback of the same batch never fires.
// A refactor to "collect the due timers, then fire them" breaks that, and the fake's timeouts depend on it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpVirtualClockEdgeTest, "VaRest.Http.Clock.VirtualEdges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpVirtualClockEdgeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::VaRestHttpClockTestPrivate;
	{
		TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
		FLogRef Log = MakeShared<FLog, ESPMode::ThreadSafe>();
		FLogRef Victim = MakeShared<FLog, ESPMode::ThreadSafe>();
		Clock->SetTimer(1.0, [Log, Victim, Clock] { Log->Fired.Add(TEXT("first")); Clock->ClearTimer(Victim->Handle); });
		Victim->Handle = Clock->SetTimer(1.0, [Log] { Log->Fired.Add(TEXT("victim")); });
		Clock->Advance(1.0);
		TestEqualSensitive(TEXT("a callback clears a timer due in the same batch: it never fires"), Log->Joined(), FString(TEXT("first")));
		TestEqual(TEXT("... and nothing is left pending"), Clock->NumPendingTimers(), 0);
	}
	{
		TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
		FLogRef Log = MakeShared<FLog, ESPMode::ThreadSafe>();
		Clock->SetTimer(1.0, [Log, Clock] { Log->Fired.Add(TEXT("outer")); Clock->SetTimer(-5.0, [Log] { Log->Fired.Add(TEXT("inner")); }); });
		Clock->SetTimer(1.0, [Log] { Log->Fired.Add(TEXT("sibling")); });
		Clock->Advance(1.0);
		TestEqualSensitive(TEXT("a timer set (with a negative delay) while firing is due now: same Advance, after older ties"), Log->Joined(), FString(TEXT("outer,sibling,inner")));
	}
	{
		TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
		FLogRef Log = MakeShared<FLog, ESPMode::ThreadSafe>();
		Clock->SetTimer(0.0, [Log] { Log->Fired.Add(TEXT("zero")); });
		Clock->SetTimer(-1.0, [Log] { Log->Fired.Add(TEXT("negative")); });
		FVaRestTimerHandle Later = Clock->SetTimer(0.5, [Log] { Log->Fired.Add(TEXT("later")); });
		TestEqual(TEXT("nothing fires before an Advance"), Log->Fired.Num(), 0);
		Clock->Advance(0.0);
		TestEqualSensitive(TEXT("Advance(0) fires what is due now, in creation order, and only that"), Log->Joined(), FString(TEXT("zero,negative")));
		TestEqual(TEXT("... and time did not move"), Clock->Now(), 0.0);
		TestEqual(TEXT("the later timer is still pending"), Clock->NumPendingTimers(), 1);
		Clock->Advance(-3.0);
		TestEqual(TEXT("a negative Advance does not move time backwards"), Clock->Now(), 0.0);
		Clock->Advance(0.5);
		TestEqualSensitive(TEXT("the later timer fires at its time"), Log->Joined(), FString(TEXT("zero,negative,later")));

		// Clearing a handle that already fired is a no-op that still invalidates it, and leaves other timers alone
		FLogRef Other = MakeShared<FLog, ESPMode::ThreadSafe>();
		Clock->SetTimer(10.0, [Other] { Other->Fired.Add(TEXT("other")); });
		TestTrue(TEXT("a fired handle is still 'valid' until cleared"), Later.IsValid());
		Clock->ClearTimer(Later);
		TestFalse(TEXT("clearing a fired handle invalidates it"), Later.IsValid());
		TestEqual(TEXT("... and does not touch another timer"), Clock->NumPendingTimers(), 1);
		FVaRestTimerHandle None;
		Clock->ClearTimer(None);
		TestEqual(TEXT("clearing an invalid handle is a no-op"), Clock->NumPendingTimers(), 1);
		Clock->Advance(10.0);
		TestEqualSensitive(TEXT("the other timer still fires"), Other->Joined(), FString(TEXT("other")));
	}
	{
		TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
		FLogRef Log = MakeShared<FLog, ESPMode::ThreadSafe>();
		Log->Handle = Clock->SetTimer(1.0, [Log, Clock] { Log->Fired.Add(TEXT("self")); Clock->ClearTimer(Log->Handle); });
		Clock->Advance(1.0);
		TestEqualSensitive(TEXT("a timer clearing its own handle from its callback is a no-op"), Log->Joined(), FString(TEXT("self")));
		TestFalse(TEXT("... and the handle ends invalid"), Log->Handle.IsValid());
		TestEqual(TEXT("... with nothing pending"), Clock->NumPendingTimers(), 0);
	}
	{
		// Review M-2: now + delay saturates; without it a "never" timer wraps negative once Now() passes 8.2e9 s
		TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
		for (int32 i = 0; i < 9; ++i)
		{
			Clock->Advance(FVaRestVirtualClock::MaxSeconds);
		}
		FLogRef Log = MakeShared<FLog, ESPMode::ThreadSafe>();
		Clock->SetTimer(FVaRestVirtualClock::MaxSeconds, [Log] { Log->Fired.Add(TEXT("never")); });
		Clock->SetTimer(std::numeric_limits<double>::quiet_NaN(), [Log] { Log->Fired.Add(TEXT("nan")); });
		Clock->Advance(1.0);
		TestEqual(TEXT("'never' timers set late in a long-running clock do not wrap and fire"), Log->Fired.Num(), 0);
	}
	return true;
}

// The real clock: a core-ticker Tick drives it. Two zero-delay timers are due in the same Tick.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRealClockEdgeTest, "VaRest.Http.Clock.RealEdges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRealClockEdgeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::VaRestHttpClockTestPrivate;
	TSharedRef<FVaRestRealClock, ESPMode::ThreadSafe> Clock = FVaRestRealClock::Get();
	FLogRef Log = MakeShared<FLog, ESPMode::ThreadSafe>();
	FLogRef Victim = MakeShared<FLog, ESPMode::ThreadSafe>();
	FLogRef SelfClear = MakeShared<FLog, ESPMode::ThreadSafe>();
	Clock->SetTimer(0.0, [Log, Victim, Clock] { Log->Fired.Add(TEXT("first")); Clock->ClearTimer(Victim->Handle); });
	Victim->Handle = Clock->SetTimer(0.0, [Log] { Log->Fired.Add(TEXT("victim")); });
	SelfClear->Handle = Clock->SetTimer(-2.0, [Log, SelfClear, Clock] { Log->Fired.Add(TEXT("negative")); Clock->ClearTimer(SelfClear->Handle); });
	FVaRestTimerHandle NanHandle = Clock->SetTimer(std::numeric_limits<double>::quiet_NaN(), [Log] { Log->Fired.Add(TEXT("nan")); });
	if (!TestTrue(TEXT("the due timers fire from one pump"), VaRest::Http::Testing::PumpUntil([Log] { return Log->Fired.Num() >= 2; }, 2.0, true)))
	{
		Clock->ClearTimer(NanHandle);
		return false;
	}
	VaRest::Http::Testing::PumpUntil([] { return false; }, 0.1, true);
	TestEqualSensitive(TEXT("a sibling cleared by a callback in the same Tick never fires; a negative delay is due at once; NaN is never"), Log->Joined(), FString(TEXT("first,negative")));
	TestFalse(TEXT("a timer clearing its own handle: the handle ends invalid"), SelfClear->Handle.IsValid());
	Clock->ClearTimer(SelfClear->Handle); // already fired: a no-op
	Clock->ClearTimer(NanHandle);
	TestFalse(TEXT("clearing a pending timer invalidates its handle"), NanHandle.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogCaptureTest, "VaRest.Http.Testing.LogCapture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogCaptureTest::RunTest(const FString& Parameters)
{
	VaRest::Http::Testing::FLogCapture Capture;
	UE_LOG(LogVaRestHttp, Log, TEXT("capture-probe game thread"));
	// Not Wait(): a waited task may be retracted and run on the waiting thread (TaskPrivate.cpp:75-100), which is the game thread here
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bLogged = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	UE::Tasks::Launch(TEXT("VaRest.Test.Log"), [bLogged] { UE_LOG(LogVaRestHttp, Log, TEXT("capture-probe worker")); *bLogged = true; });
	VaRest::Http::Testing::PumpUntil([bLogged] { return bLogged->load(); });
	TestTrue(TEXT("game-thread lines are captured"), Capture.Contains(TEXT("capture-probe game thread")));
	TestTrue(TEXT("worker lines are captured at once (CanBeUsedOnMultipleThreads)"), Capture.Contains(TEXT("capture-probe worker")));
	TestEqual(TEXT("Count"), Capture.Count(TEXT("capture-probe")), 2);
	return true;
}

#endif
