// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
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
	TestTrue(TEXT("worker lines are captured at once (CanBeUsedOnAnyThread)"), Capture.Contains(TEXT("capture-probe worker")));
	TestEqual(TEXT("Count"), Capture.Count(TEXT("capture-probe")), 2);
	return true;
}

#endif
