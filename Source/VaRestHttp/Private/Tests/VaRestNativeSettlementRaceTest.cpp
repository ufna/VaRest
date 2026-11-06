// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "VaRestNativeLifetime.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedTerminalRaceTest, "VaRest.Http.Managed.TerminalAgainstFinalTail",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedTerminalRaceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("concurrent native settlement requires multithreading; schedule not exercised"));
		return true;
	}
	FEventRef Kick;
	FEventRef Done;
	std::atomic<FManagedOperationSettlement*> Active{nullptr};
	std::atomic<bool> Stop{false};
	auto Worker = Async(EAsyncExecution::Thread, [&] {
		for (;;)
		{
			Kick->Wait();
			if (Stop.load(std::memory_order_acquire))
				break;
			Active.load(std::memory_order_acquire)->SettleLogicalTail();
			Done->Trigger();
		}
	});
	bool Correct = true;
	for (int32 Round = 0; Round < 512; ++Round)
	{
		auto Gate = MakeShared<FClientAdmissionGate, ESPMode::ThreadSafe>();
		FClientWorkLease Work;
		Gate->TryAcquire(Work);
		FManagedOperationSettlement Settlement(MoveTemp(Work));
		Gate->ClosePermanently();
		Settlement.CloseChildAdmission();
		Active.store(&Settlement, std::memory_order_release);
		Kick->Trigger();
		Settlement.MarkLogicalTerminalSettled(); // races the last worker decrement, rather than following its Wait
		Done->Wait();
		const bool Released = TestEqual(TEXT("terminal/final-tail race releases admission without another event"), Gate->NumOwnedOperations(), uint32(0));
		Correct &= Released;
		if (!Released)
			Settlement.MarkLogicalTerminalSettled(); // failure-only teardown, AFTER the discriminating assertion
		Active.store(nullptr, std::memory_order_release);
	}
	Stop.store(true, std::memory_order_release);
	Kick->Trigger();
	Worker.Wait();
	// Racing runtime schedules complement the seq_cst proof; a pass cannot prove all weak-memory executions.
	return Correct;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedTwoFinalWorkersTest, "VaRest.Http.Managed.ConcurrentFinalWorkers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedTwoFinalWorkersTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("two final native workers require multithreading; schedule not exercised"));
		return true;
	}
	FEventRef KickLogical;
	FEventRef KickPhysical;
	FEventRef Ready(EEventMode::ManualReset);
	FEventRef Start(EEventMode::ManualReset);
	FEventRef Done(EEventMode::ManualReset);
	std::atomic<FManagedOperationSettlement*> Active{nullptr};
	std::atomic<uint32> Arrived{0};
	std::atomic<uint32> Completed{0};
	std::atomic<bool> Stop{false};
	const auto WorkLoop = [&](FEventRef& Kick, bool Physical) {
		for (;;)
		{
			Kick->Wait();
			if (Stop.load(std::memory_order_acquire))
				break;
			if (Arrived.fetch_add(1, std::memory_order_acq_rel) == 1)
				Ready->Trigger();
			Start->Wait();
			FManagedOperationSettlement* Settlement = Active.load(std::memory_order_acquire);
			if (Physical)
				Settlement->SettleDispatch();
			else
				Settlement->SettleLogicalTail();
			if (Completed.fetch_add(1, std::memory_order_acq_rel) == 1)
				Done->Trigger();
		}
	};
	// Dedicated owned threads avoid a two-party barrier starving a single-worker task pool.
	auto Logical = Async(EAsyncExecution::Thread, [&] { WorkLoop(KickLogical, false); });
	auto Physical = Async(EAsyncExecution::Thread, [&] { WorkLoop(KickPhysical, true); });
	bool Correct = true;
	for (int32 Round = 0; Round < 512; ++Round)
	{
		auto Gate = MakeShared<FClientAdmissionGate, ESPMode::ThreadSafe>();
		FClientWorkLease Work;
		Gate->TryAcquire(Work);
		FManagedOperationSettlement Settlement(MoveTemp(Work));
		Settlement.TryAddDispatchSettlement();
		Gate->ClosePermanently();
		Settlement.CloseChildAdmission();
		Settlement.MarkLogicalTerminalSettled();
		Ready->Reset();
		Start->Reset();
		Done->Reset();
		Arrived.store(0, std::memory_order_release);
		Completed.store(0, std::memory_order_release);
		Active.store(&Settlement, std::memory_order_release);
		KickLogical->Trigger();
		KickPhysical->Trigger();
		Ready->Wait();
		Start->Trigger();
		Done->Wait();
		const bool Released = TestEqual(TEXT("concurrent last logical and physical workers release without a retry"), Gate->NumOwnedOperations(), uint32(0));
		Correct &= Released;
		if (!Released)
			Settlement.MarkLogicalTerminalSettled(); // recorded failure cleanup cannot make the earlier oracle pass
		Active.store(nullptr, std::memory_order_release);
	}
	Stop.store(true, std::memory_order_release);
	KickLogical->Trigger();
	KickPhysical->Trigger();
	Logical.Wait();
	Physical.Wait();
	return Correct;
}

#endif
