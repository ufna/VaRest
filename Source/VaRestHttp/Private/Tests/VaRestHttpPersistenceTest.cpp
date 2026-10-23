// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS
#include "VaRestPersistence.h"
#include "VaRestHttpTesting.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformFileManager.h"
#include "Tasks/Task.h"
#include "UObject/Package.h"
#include "VaRestPersistenceWait.h"
#include "VaRestClientState.h"
#include "VaRestHttpSettings.h"
#if PLATFORM_LINUX
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cerrno>
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestPersistenceOrderingTest, "VaRest.Http.Persistence.WorkerOrdering",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestPersistenceOrderingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FScopedPersistenceTest Disk;
	const FString Path = FPaths::Combine(Disk.Root, TEXT("ordered.json"));
	struct FObserved
	{
		TArray<int32> Order;
		TArray<bool> Ok;
		TArray<uint8> Read;
	};
	const auto Seen = MakeShared<FObserved, ESPMode::ThreadSafe>();
	const uint64 Before = PersistenceCounters().OperationsOnGameThread.load();
	const uint64 Launched = PersistenceCounters().Launched.load();
	TestTrue(TEXT("write accepted"), LaunchPersistence([Path] { return WriteNewFile(Path, TArray<uint8>{97}); },
	                                                   [Seen](FIoResult&& Result)
	                                                   {
		                                                   Seen->Order.Add(1);
		                                                   Seen->Ok.Add(Result.bOk);
	                                                   }));
	TestTrue(TEXT("read accepted"), LaunchPersistence([Path] { return ReadBoundedFile(Path, 1); },
	                                                  [Seen](FIoResult&& Result)
	                                                  {
		                                                  Seen->Order.Add(2);
		                                                  Seen->Ok.Add(Result.bOk);
		                                                  Seen->Read = MoveTemp(Result.Bytes);
	                                                  }));
	TestTrue(TEXT("delete accepted"), LaunchPersistence([Path] { return VaRest::Http::DeleteFile(Path); },
	                                                    [Seen](FIoResult&& Result)
	                                                    {
		                                                    Seen->Order.Add(3);
		                                                    Seen->Ok.Add(Result.bOk);
	                                                    }));
	TestTrue(TEXT("miss accepted"), LaunchPersistence([Path] { return ReadBoundedFile(Path, 1); },
	                                                  [Seen](FIoResult&& Result)
	                                                  {
		                                                  Seen->Order.Add(4);
		                                                  Seen->Ok.Add(Result.bOk);
	                                                  }));
	if (!TestTrue(TEXT("drain"), Disk.Drain()))
		return false;
	TestTrue(TEXT("serial callback order"), Seen->Order == TArray<int32>({1, 2, 3, 4}));
	TestTrue(TEXT("write read delete missing"), Seen->Ok == TArray<bool>({true, true, true, false}));
	TestTrue(TEXT("exact bytes"), Seen->Read == TArray<uint8>({97}));
	TestEqual(TEXT("four jobs"), PersistenceCounters().Launched.load() - Launched, uint64(4));
	TestEqual(TEXT("no disk operation on GT"), PersistenceCounters().OperationsOnGameThread.load(), Before);
	return true;
}

namespace VaRest::Http::PersistenceExecutorTestPrivate
{
	class FWaitStage final : public IVaRestStage
	{
		public:
		virtual FName GetStageName() const override
		{
			return TEXT("PersistenceHold");
		}
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Seen = Exchange;
			auto Wait = FPersistenceWait::Begin(Exchange, MoveTemp(OnOutcome));
			if (!Wait->IsPending())
				return;
			if (bImmediateFailure)
			{
				FVaRestAttemptOutcome Failure;
				Failure.Failure = EVaRestTransportFailure::Other;
				Failure.FailureDetail = TEXT("PersistenceStopped");
				Wait->Finish(MoveTemp(Failure));
				return;
			}
			const auto Gate = Barrier;
			bAdmitted = LaunchPersistence(
			    [Gate]
			    {
				    Testing::WaitPersistenceBarrier(Gate);
				    FIoResult Result;
				    Result.bOk = true;
				    return Result;
			    },
			    [Wait, Exchange, Next, bContinue = bResume](FIoResult&&) mutable
			    {
				    if (bContinue)
				    {
					    auto Callback = Wait->Resume();
					    if (Callback)
						    Next.Proceed(Exchange, MoveTemp(Callback));
				    }
				    else
				    {
					    FVaRestAttemptOutcome Outcome;
					    Outcome.Status = 204;
					    Wait->Finish(MoveTemp(Outcome));
					    // A stale/double result must not answer twice.
					    FVaRestAttemptOutcome Again;
					    Again.Status = 200;
					    Wait->Finish(MoveTemp(Again));
				    }
			    },
			    64);
			if (!bAdmitted)
			{
				FVaRestAttemptOutcome Failure;
				Failure.Failure = EVaRestTransportFailure::Other;
				Failure.FailureDetail = TEXT("PersistenceStopped");
				Wait->Finish(MoveTemp(Failure));
			}
		}
		TSharedRef<Testing::FPersistenceBarrier, ESPMode::ThreadSafe> Barrier = MakeShared<Testing::FPersistenceBarrier, ESPMode::ThreadSafe>();
		TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe> Seen;
		bool bImmediateFailure = false, bResume = false, bAdmitted = false;
	};

	// Now can advance independently from delivery, as with a delayed real ticker.
	class FHeldTimerClock final : public IVaRestClock
	{
		public:
		virtual double Now() const override
		{
			return Seconds;
		}
		virtual FVaRestTimerHandle SetTimer(double DelaySeconds, TUniqueFunction<void()>&& Callback) override
		{
			const uint64 Ticket = NewTimerId();
			Pending.Add(Ticket, MoveTemp(Callback));
			return {Ticket};
		}
		virtual void ClearTimer(FVaRestTimerHandle& Handle) override
		{
			Pending.Remove(Handle.Id);
			Handle.Invalidate();
		}
		double Seconds = 0;
		TMap<uint64, TUniqueFunction<void()>> Pending;
	};

	FVaRestRetryPolicy DeadlinePolicy()
	{
		auto Policy = FVaRestRetryPolicy::Disabled();
		Policy.Deadline = 1.0f;
		return Policy;
	}
} // namespace VaRest::Http::PersistenceExecutorTestPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestPersistenceWaitTest, "VaRest.Http.Persistence.WaitCancelDeadline",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestPersistenceWaitTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PersistenceExecutorTestPrivate;
	for (int32 Mode = 0; Mode < 2; ++Mode)
	{
		FScopedPersistenceTest Disk;
		FTestRig Rig;
		const auto Probe = MakeShared<FOutcomeProbe, ESPMode::ThreadSafe>();
		const auto Hold = MakeShared<FWaitStage, ESPMode::ThreadSafe>();
		ON_SCOPE_EXIT
		{
			Hold->Barrier->Released = true;
		};
		Rig.Client.Use(Probe, EVaRestStageSlot::Outer).Use(Hold, EVaRestStageSlot::Outer).WithRetry(DeadlinePolicy());
		const auto Outcome = MakeOutcome<void>();
		const auto Handle = Observe(Rig.Client.Get<void>(TEXT("/wait")), Outcome).Send();
		if (!TestTrue(TEXT("worker entered"), PumpUntil([Hold] { return Hold->Barrier->Entered.load(); })))
			return false;
		if (Mode == 0)
		{
			Handle.Cancel();
			TestEqual(TEXT("cancel answers outer probe before worker release"), Probe->Total(Handle.GetId()), 1);
			TestEqual(TEXT("Cancel does not deliver inline"), Outcome->CompleteCalls.load(), 0);
		}
		Rig.Clock->Advance(2.0);
		TestTrue(TEXT("outcome prompt while IO is held"), Rig.Wait(Outcome));
		TestEqual(TEXT("one outer outcome while blocked"), Probe->Total(Handle.GetId()), 1);
		TestEqual(TEXT("one error"), Outcome->ErrorCalls.load(), 1);
		TestEqual(TEXT("correct terminal reason"), Outcome->Error.Kind, Mode == 0 ? EVaRestErrorKind::Canceled : EVaRestErrorKind::Timeout);
		TestEqual(TEXT("nothing sent"), Rig.Fake->NumCalls(), 0);
		TestEqual(TEXT("reservation retained for held IO"), PersistencePendingBytesForTest(), int64(64));
		Hold->Barrier->Released = true;
		TestTrue(TEXT("drain after release"), Rig.Drain());
		TestFalse(TEXT("barrier did not time out"), Hold->Barrier->TimedOut.load());
		TestEqual(TEXT("late handback cannot answer twice"), Probe->Total(Handle.GetId()), 1);
		TestEqual(TEXT("one completion after handback"), Outcome->CompleteCalls.load(), 1);
		TestEqual(TEXT("timer disarmed"), Rig.Clock->NumPendingTimers(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestPersistenceHandoffTest, "VaRest.Http.Persistence.HandoffRechecks",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestPersistenceHandoffTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PersistenceExecutorTestPrivate;
	for (int32 Mode = 0; Mode < 3; ++Mode)
	{
		FScopedPersistenceTest Disk;
		FTestRig Rig;
		const auto Clock = MakeShared<FHeldTimerClock, ESPMode::ThreadSafe>();
		const auto Probe = MakeShared<FOutcomeProbe, ESPMode::ThreadSafe>();
		const auto Hold = MakeShared<FWaitStage, ESPMode::ThreadSafe>();
		ON_SCOPE_EXIT
		{
			Hold->Barrier->Released = true;
		};
		Rig.Client.WithClock(Clock).WithRetry(DeadlinePolicy()).Use(Probe, EVaRestStageSlot::Outer).Use(Hold, EVaRestStageSlot::Outer);
		UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
		const auto Outcome = MakeOutcome<void>();
		const auto Handle = Observe(Rig.Client.Get<void>(TEXT("/handoff")).Owner(Owner), Outcome).Send();
		if (!TestTrue(TEXT("worker entered"), PumpUntil([Hold] { return Hold->Barrier->Entered.load(); })))
			return false;
		if (Mode == 0)
			Clock->Seconds = 2.0;
		else if (Mode == 1)
			Owner->MarkAsGarbage();
		else
		{
			const auto Exchange = Hold->Seen.Pin();
			if (!TestTrue(TEXT("held exchange"), Exchange.IsValid()))
				return false;
			Internals(*Exchange).bCancelRequested = true; // any-thread signal before its GT Cancel task
		}
		TestEqual(TEXT("no timer delivered"), Probe->Total(Handle.GetId()), 0);
		Hold->Barrier->Released = true;
		TestTrue(TEXT("handoff drained"), Rig.Drain());
		TestEqual(TEXT("one outer outcome"), Probe->Total(Handle.GetId()), 1);
		TestEqual(TEXT("no wire"), Rig.Fake->NumCalls(), 0);
		TestEqual(TEXT("owner suppression"), Outcome->CompleteCalls.load(), Mode == 1 ? 0 : 1);
		if (Mode != 1)
			TestEqual(TEXT("handoff reason"), Outcome->Error.Kind, Mode == 0 ? EVaRestErrorKind::Timeout : EVaRestErrorKind::Canceled);
		TestEqual(TEXT("held timer cleared"), Clock->Pending.Num(), 0);
		TestFalse(TEXT("request inactive"), Handle.IsActive());
		TestEqual(TEXT("request removed from in-flight accounting"), FClientAccess::State(Rig.Client)->NumInFlight(), 0);
		TestFalse(TEXT("barrier timeout"), Hold->Barrier->TimedOut.load());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestPersistenceResumeTest, "VaRest.Http.Persistence.ResumeAndSuccessfulCompletion",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestPersistenceResumeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PersistenceExecutorTestPrivate;
	FScopedPersistenceTest Disk;
	FTestRig Rig;
	const auto Probe = MakeShared<FOutcomeProbe, ESPMode::ThreadSafe>();
	const auto Hold = MakeShared<FWaitStage, ESPMode::ThreadSafe>();
	Hold->bResume = true;
	ON_SCOPE_EXIT
	{
		Hold->Barrier->Released = true;
	};
	Rig.Client.Use(Probe, EVaRestStageSlot::Outer).Use(Hold, EVaRestStageSlot::Outer);
	Rig.Fake->Enqueue(FVaRestFakeResponse::Text(204, TEXT("")));
	const auto Outcome = MakeOutcome<void>();
	const auto Handle = Observe(Rig.Client.Get<void>(TEXT("/resume")), Outcome).Send();
	if (!TestTrue(TEXT("worker entered"), PumpUntil([Hold] { return Hold->Barrier->Entered.load(); })))
		return false;
	const auto Exchange = Hold->Seen.Pin();
	if (!TestTrue(TEXT("held exchange"), Exchange.IsValid()))
		return false;
	Hold->Barrier->Released = true;
	TestTrue(TEXT("resumed request drains"), Rig.Drain());
	TestEqual(TEXT("one successful completion"), Outcome->SuccessCalls.load(), 1);
	TestEqual(TEXT("one outer outcome"), Probe->Total(Handle.GetId()), 1);
	TestEqual(TEXT("one real fake dispatch"), Rig.Fake->NumCalls(), 1);
	TestFalse(TEXT("success does not cancel best-effort persistence"), Internals(*Exchange).PersistenceAbort->load());
	TestEqual(TEXT("permit released"), Internals(*Exchange).PersistenceCompletionTicket, uint64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestPersistenceDrainTest, "VaRest.Http.Persistence.DrainPublicationOrdering",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestPersistenceDrainTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FScopedPersistenceTest Disk;
	FTestRig Rig;
	const auto Gate = MakeShared<FPersistenceBarrier, ESPMode::ThreadSafe>();
	ON_SCOPE_EXIT
	{
		Gate->Released = true;
	};
	const auto Delivered = MakeShared<int32, ESPMode::ThreadSafe>(0);
	const uint64 Before = PersistenceCounters().HandedBack.load();
	TestTrue(TEXT("admitted"), LaunchWithPublicationBarrier(
	                               []
	                               {
		                               FIoResult Result;
		                               Result.bOk = true;
		                               return Result;
	                               },
	                               [Delivered](FIoResult&&) { ++*Delivered; }, Gate));
	if (!TestTrue(TEXT("result ready but publication held"), PumpUntil([Gate] { return Gate->Entered.load(); })))
		return false;
	TestEqual(TEXT("handback cannot be counted before result publication"), PersistenceCounters().HandedBack.load(), Before);
	TestFalse(TEXT("fixture cannot drain unpublished result"), Disk.Drain(0.01));
	TestFalse(TEXT("rig cannot drain unpublished result"), Rig.Drain(0.01));
	TestEqual(TEXT("nothing delivered while held"), *Delivered, 0);
	Gate->Released = true;
	TestTrue(TEXT("drains after actual publication"), Rig.Drain());
	TestEqual(TEXT("one callback"), *Delivered, 1);
	TestEqual(TEXT("one counted handback"), PersistenceCounters().HandedBack.load(), Before + 1);
	TestTrue(TEXT("quiescent"), PersistenceIdle());
	TestFalse(TEXT("no timeout"), Gate->TimedOut.load());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestPersistenceBoundsTest, "VaRest.Http.Persistence.AdmissionAndFullMapCleanup",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestPersistenceBoundsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FScopedPersistenceRuntime Runtime;
	FScopedPersistenceTest Disk;
	const auto Gate = MakeShared<FPersistenceBarrier, ESPMode::ThreadSafe>();
	ON_SCOPE_EXIT
	{
		Gate->Released = true;
	};
	struct FSeen
	{
		int32 Calls = 0;
		bool bCleanup = false, bOverflow = false, bReservation = false, bNestedPump = false;
	};
	const auto Seen = MakeShared<FSeen, ESPMode::ThreadSafe>();
	constexpr int64 Cap = 128 * 1024 * 1024;
	TestFalse(TEXT("negative charge refused"), LaunchPersistence([] { return FIoResult(); }, [](FIoResult&&) {}, -1));
	TestFalse(TEXT("over cap refused"), LaunchPersistence([] { return FIoResult(); }, [](FIoResult&&) {}, Cap + 1));
	TestTrue(TEXT("cap admitted"), LaunchPersistence(
	                                   [Gate]
	                                   {
		                                   WaitPersistenceBarrier(Gate);
		                                   return FIoResult();
	                                   },
	                                   [Seen](FIoResult&&)
	                                   {
		                                   ++Seen->Calls;
		                                   Seen->bReservation = PersistencePendingBytesForTest() == Cap;
		                                   Seen->bOverflow = LaunchPersistence([] { return FIoResult(); }, [](FIoResult&&) {}, 1);
		                                   Seen->bCleanup = LaunchPersistence([] { return FIoResult(); }, [Seen](FIoResult&&) { ++Seen->Calls; }, 0);
		                                   PumpPersistence();
		                                   Seen->bNestedPump = Seen->Calls != 1;
	                                   },
	                                   Cap));
	if (!TestTrue(TEXT("first worker entered"), PumpUntil([Gate] { return Gate->Entered.load(); })))
		return false;
	TestFalse(TEXT("byte cap refuses another byte"), LaunchPersistence([] { return FIoResult(); }, [](FIoResult&&) {}, 1));
	for (int32 Index = 1; Index < 8192; ++Index)
		if (!TestTrue(TEXT("fill actual callback slot"), LaunchPersistence([] { return FIoResult(); }, [Seen](FIoResult&&) { ++Seen->Calls; })))
			return false;
	TestFalse(TEXT("8193rd callback refused"), LaunchPersistence([] { return FIoResult(); }, [](FIoResult&&) {}));
	Gate->Released = true;
	TestTrue(TEXT("full map drained"), Disk.Drain());
	TestTrue(TEXT("callback removal permits mandatory zero-byte cleanup"), Seen->bCleanup);
	TestTrue(TEXT("reservation held throughout callback"), Seen->bReservation);
	TestFalse(TEXT("callback cannot reuse charged bytes"), Seen->bOverflow);
	TestFalse(TEXT("nested pump does not deliver callbacks inline"), Seen->bNestedPump);
	TestEqual(TEXT("accepted callbacks all delivered"), Seen->Calls, 8193);
	TestEqual(TEXT("charge released"), PersistencePendingBytesForTest(), int64(0));
	TestFalse(TEXT("no barrier timeout"), Gate->TimedOut.load());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestPersistencePermitsTest, "VaRest.Http.Persistence.ContinuationAndCompletionCapacity",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestPersistencePermitsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PersistenceExecutorTestPrivate;
	FScopedPersistenceRuntime Runtime;
	FScopedPersistenceTest Disk;
	TArray<uint64> Tickets;
	const auto Seen = MakeShared<TArray<int32>, ESPMode::ThreadSafe>();
	const auto PollCalls = MakeShared<int32, ESPMode::ThreadSafe>(0);
	const auto RefusedInside = MakeShared<bool, ESPMode::ThreadSafe>(false);
	const auto PollReady = MakeShared<bool, ESPMode::ThreadSafe>(false);
	for (int32 Index = 0; Index < 8192; ++Index)
	{
		const uint64 Ticket = AcquirePersistenceCompletion();
		if (!TestTrue(TEXT("completion permit"), Ticket != 0))
			return false;
		Tickets.Add(Ticket);
		TestTrue(TEXT("poll permit"), PostPersistencePoll(
		                                  [PollCalls, RefusedInside, PollReady]
		                                  {
			                                  ++*PollCalls;
			                                  if (*PollCalls == 1)
				                                  *RefusedInside = !PostPersistenceContinuation([] {});
			                                  return *PollReady;
		                                  }));
	}
	TestEqual(TEXT("completion lane bounded"), AcquirePersistenceCompletion(), uint64(0));
	TestFalse(TEXT("deferred lane bounded"), PostPersistenceContinuation([] {}));
	TestTrue(TEXT("independent IO lane"), LaunchPersistence([] { return FIoResult(); }, [](FIoResult&&) {}));
	// Permit refusal creates no accepted wait or IO. Its Config answer remains normally asynchronous.
	FTestRig Rig;
	const auto Hold = MakeShared<FWaitStage, ESPMode::ThreadSafe>();
	Rig.Client.Use(Hold, EVaRestStageSlot::Outer);
	const auto Outcome = MakeOutcome<void>();
	const auto Handle = Observe(Rig.Client.Get<void>(TEXT("/refused")), Outcome).Send();
	const auto State = FClientAccess::State(Rig.Client);
	if (!TestTrue(TEXT("client state"), State.IsValid()))
		return false;
	TestFalse(TEXT("no job on refused permit"), Hold->bAdmitted);
	const auto RefusedExchange = Hold->Seen.Pin();
	if (!TestTrue(TEXT("refused exchange waiting for ordinary delivery"), RefusedExchange.IsValid()))
		return false;
	TestFalse(TEXT("refusal has no persistence ownership"), Internals(*RefusedExchange).bPersistenceOwned);
	TestEqual(TEXT("refusal has no completion permit"), Internals(*RefusedExchange).PersistenceCompletionTicket, uint64(0));
	TestFalse(TEXT("refusal installs no wait interrupt"), bool(Internals(*RefusedExchange).Interrupt));
	TestEqual(TEXT("no inline callback"), Outcome->CompleteCalls.load(), 0);
	TestEqual(TEXT("ordinary refused wrapper"), State->PendingApiCompletionWrappers, 1);
	PumpPersistence();
	TestEqual(TEXT("poll callbacks retain their capacity while running"), *PollCalls, 8192);
	TestTrue(TEXT("reentrant deferred admission bounded"), *RefusedInside);
	TestFalse(TEXT("unready polls and permits keep runtime nonidle"), PersistenceIdle());
	*PollReady = true;
	for (uint64 Ticket : Tickets)
		ReleasePersistenceCompletion(Ticket);
	TestTrue(TEXT("ordinary refusal delivered"), Rig.Wait(Outcome));
	TestEqual(TEXT("Config refusal"), Outcome->Error.Kind, EVaRestErrorKind::Config);
	TestFalse(TEXT("capacity refusal is nonretryable"), Outcome->Error.bRetryable);
	TestEqual(TEXT("ordinary wrapper retired"), State->PendingApiCompletionWrappers, 0);
	TestTrue(TEXT("capacity drains"), Rig.Drain());
	const uint64 First = AcquirePersistenceCompletion(), Second = AcquirePersistenceCompletion();
	SetPersistenceCompletion(Second, [Seen] { Seen->Add(2); });
	SetPersistenceCompletion(First, [Seen] { Seen->Add(99); });
	SetPersistenceCompletion(First, [Seen] { Seen->Add(1); });
	PumpPersistence();
	TestTrue(TEXT("ready completions ordered and replacement is single"), *Seen == TArray<int32>({1, 2}));
	ReleasePersistenceCompletion(First);
	ReleasePersistenceCompletion(Second);
	TestTrue(TEXT("completion releases idempotent"), PersistenceIdle());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestPersistenceStopTest, "VaRest.Http.Persistence.IsolatedStop",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestPersistenceStopTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	const FStopProbe Probe = RunIsolatedStopProbe();
	TestFalse(TEXT("ordinary callbacks suppressed"), Probe.bOrdinaryCalled);
	TestTrue(TEXT("accepted commit accounted"), Probe.bCommitCalled);
	TestFalse(TEXT("unrelated task not pumped by Stop"), Probe.bUnrelatedCalledAtStop);
	TestFalse(TEXT("control continuation cannot admit more IO"), Probe.bAcceptedAfterStop);
	TestFalse(TEXT("deferred callback discarded"), Probe.bDeferredCalled);
	TestFalse(TEXT("owned completion discarded"), Probe.bOwnedCalled);
	TestTrue(TEXT("own slots and reservations drained"), Probe.bClean);
	TestTrue(TEXT("ordinary test pump eventually runs witness"), PumpUntil([Seen = Probe.Unrelated] { return Seen->load(); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestPersistenceOwnedStopTest, "VaRest.Http.Persistence.OwnedWaitShutdown",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestPersistenceOwnedStopTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PersistenceExecutorTestPrivate;
	for (int32 Mode = 0; Mode < 2; ++Mode)
	{
		TWeakPtr<FClientState, ESPMode::ThreadSafe> WeakState;
		const auto Witness = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
		{
			FScopedPersistenceRuntime Runtime;
			FTestRig Rig;
			const auto State = FClientAccess::State(Rig.Client);
			WeakState = State;
			if (!TestTrue(TEXT("state exists"), State.IsValid()))
				return false;
			const auto Probe = MakeShared<FOutcomeProbe, ESPMode::ThreadSafe>();
			const auto Hold = MakeShared<FWaitStage, ESPMode::ThreadSafe>();
			Hold->bImmediateFailure = Mode == 0;
			ON_SCOPE_EXIT
			{
				Hold->Barrier->Released = true;
			};
			Rig.Client.Use(Probe, EVaRestStageSlot::Outer).Use(Hold, EVaRestStageSlot::Outer);
			const auto Outcome = MakeOutcome<void>();
			const auto Handle = Observe(Rig.Client.Get<void>(TEXT("/stop")), Outcome).Send();
			if (Mode == 1 && !TestTrue(TEXT("held worker"), PumpUntil([Hold] { return Hold->Barrier->Entered.load(); })))
				return false;
			TestEqual(TEXT("no callback on Send"), Outcome->CompleteCalls.load(), 0);
			TestEqual(TEXT("accepted completion has no global wrapper"), State->PendingApiCompletionWrappers, 0);
			UE::Tasks::Launch(
			    TEXT("VaRest.Persistence.StopWitness"), [Witness] { Witness->store(true); }, UE::Tasks::ETaskPriority::Normal,
			    UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
			State->CancelAll(NAME_None, EVaRestCancelReason::Shutdown);
			TestEqual(TEXT("silent cancellation settles accounting on API stack"), State->NumInFlight(), 0);
			Hold->Barrier->Released = true;
			Runtime.Stop();
			Runtime.Stop();
			TestFalse(TEXT("Stop never pumps unrelated GT task"), Witness->load());
			TestEqual(TEXT("no user callback at Stop"), Outcome->CompleteCalls.load(), 0);
			TestEqual(TEXT("one outer outcome"), Probe->Total(Handle.GetId()), 1);
			TestEqual(TEXT("no surviving accepted global wrapper"), State->PendingApiCompletionWrappers, 0);
			TestTrue(TEXT("accepted wait released exchange"), !Hold->Seen.IsValid());
			TestTrue(TEXT("runtime drained"), PersistenceIdle());
			TestFalse(TEXT("closed IO admission"), LaunchPersistence([] { return FIoResult(); }, [](FIoResult&&) {}));
			TestFalse(TEXT("closed deferral admission"), PostPersistenceContinuation([] {}));
			TestEqual(TEXT("closed permit admission"), AcquirePersistenceCompletion(), uint64(0));
		}
		TestFalse(TEXT("no client retained by an owned completion"), WeakState.IsValid());
		TestTrue(TEXT("witness still works outside Stop"), PumpUntil([Witness] { return Witness->load(); }));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestPersistenceFilesTest, "VaRest.Http.Persistence.BoundedFilesAndFailureCleanup",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestPersistenceFilesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FScopedPersistenceTest Disk;
	const auto Seen = MakeShared<TArray<FIoResult>, ESPMode::ThreadSafe>();
	const uint64 Before = PersistenceCounters().OperationsOnGameThread.load();
	TestTrue(TEXT("file cases admitted"), LaunchPersistenceResult<TArray<FIoResult>>(
	                                          [Root = Disk.Root]
	                                          {
		                                          TArray<FIoResult> Results;
		                                          const FString Path = FPaths::Combine(Root, TEXT("present"));
		                                          Results.Add(ReadBoundedFile(Path, 2));
		                                          Results.Add(WriteNewFile(Path, TArray<uint8>{1, 2}));
		                                          Results.Add(WriteNewFile(Path, TArray<uint8>{3}));
		                                          Results.Add(ReadBoundedFile(Path, 1));
		                                          Results.Add(ReadBoundedFile(Path, -1));
		                                          Results.Add(ReadWithFault(Path, 2, EFileFault::ShortRead));
		                                          Results.Add(ReadBoundedFile(Path, 2));
		                                          for (EFileFault Fault : {EFileFault::Write, EFileFault::Flush, EFileFault::Rename})
			                                          Results.Add(WriteWithFault(FPaths::Combine(Root, TEXT("failed")), TArray<uint8>{4}, Fault));
		                                          Results.Add(ListFiles(Root));
		                                          Results.Add(VaRest::Http::DeleteFile(Path));
		                                          Results.Add(VaRest::Http::DeleteFile(Path));
		                                          Results.Add(ListFiles(FPaths::Combine(Root, TEXT("missing-directory"))));
		                                          Results.Add(WriteNewFile(FPaths::Combine(Root, TEXT("sub/nested")), {}));
		                                          Results.Add(ListFiles(Root));
		                                          return Results;
	                                          },
	                                          [Seen](TArray<FIoResult>&& Results) { *Seen = MoveTemp(Results); }, 1024));
	if (!TestTrue(TEXT("files drain"), Disk.Drain()) || !TestEqual(TEXT("all file cases"), Seen->Num(), 16))
		return false;
	TestEqual(TEXT("absent input"), (*Seen)[0].Detail, FString(TEXT("Missing")));
	TestTrue(TEXT("new immutable file written"), (*Seen)[1].bOk);
	TestEqual(TEXT("refuse replacement"), (*Seen)[2].Detail, FString(TEXT("Exists")));
	TestEqual(TEXT("oversize before allocation"), (*Seen)[3].Detail, FString(TEXT("Size")));
	TestEqual(TEXT("negative bound"), (*Seen)[4].Detail, FString(TEXT("Size")));
	TestEqual(TEXT("short read refused"), (*Seen)[5].Detail, FString(TEXT("Read")));
	TestTrue(TEXT("existing bytes preserved"), (*Seen)[6].Bytes == TArray<uint8>({1, 2}));
	TestEqual(TEXT("write fault"), (*Seen)[7].Detail, FString(TEXT("Write")));
	TestEqual(TEXT("flush fault"), (*Seen)[8].Detail, FString(TEXT("Flush")));
	TestEqual(TEXT("rename fault"), (*Seen)[9].Detail, FString(TEXT("Rename")));
	TestTrue(TEXT("all failed temporary files removed"), (*Seen)[10].Names == TArray<FString>({TEXT("present")}));
	TestTrue(TEXT("delete present and absent"), (*Seen)[11].bOk && (*Seen)[12].bOk);
	TestTrue(TEXT("missing directory is empty"), (*Seen)[13].bOk && (*Seen)[13].Names.IsEmpty());
	TestTrue(TEXT("nested file created"), (*Seen)[14].bOk);
	TestTrue(TEXT("listing excludes subdirectories and descendants"), (*Seen)[15].bOk && (*Seen)[15].Names.IsEmpty());
	TestEqual(TEXT("no disk operations on GT"), PersistenceCounters().OperationsOnGameThread.load(), Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestPersistenceDisposalTest, "VaRest.Http.Persistence.ReservationThroughDisposal",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestPersistenceDisposalTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FScopedPersistenceTest Disk;
	struct FSeen
	{
		bool bDestroyed = false, bOnGT = false;
		int64 DuringCallback = 0, DuringDestruction = 0;
	};
	struct FWitness
	{
		TSharedRef<FSeen, ESPMode::ThreadSafe> Seen;
		explicit FWitness(TSharedRef<FSeen, ESPMode::ThreadSafe> InSeen) : Seen(MoveTemp(InSeen))
		{
		}
		~FWitness()
		{
			Seen->bDestroyed = true;
			Seen->bOnGT = IsInGameThread();
			if (Seen->bOnGT)
				Seen->DuringDestruction = PersistencePendingBytesForTest();
		}
	};
	const auto Seen = MakeShared<FSeen, ESPMode::ThreadSafe>();
	TestTrue(TEXT("admitted"), LaunchPersistence(
	                               []
	                               {
		                               FIoResult Result;
		                               Result.Bytes = {1, 2, 3};
		                               return Result;
	                               },
	                               [Kept = MakeUnique<FWitness>(Seen)](FIoResult&&) { Kept->Seen->DuringCallback = PersistencePendingBytesForTest(); }, 4096));
	TestTrue(TEXT("drain"), Disk.Drain());
	TestTrue(TEXT("callback captures disposed on GT"), Seen->bDestroyed && Seen->bOnGT);
	TestEqual(TEXT("bytes charged during callback"), Seen->DuringCallback, int64(4096));
	TestEqual(TEXT("bytes charged through capture disposal"), Seen->DuringDestruction, int64(4096));
	TestEqual(TEXT("bytes finally released"), PersistencePendingBytesForTest(), int64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestPersistenceHooksTest, "VaRest.Http.Persistence.DirectoryHooksAndDeferredDrain",
                                 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestPersistenceHooksTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FScopedPersistenceTest Disk;
	const auto Gate = MakeShared<FPersistenceBarrier, ESPMode::ThreadSafe>();
	const auto Seen = MakeShared<int32, ESPMode::ThreadSafe>(0);
	{
		FScopedPersistenceHooks Hooks(Disk.Root);
		Hooks.Hooks->BeforeRead = Gate;
		Hooks.Hooks->bFailCommit = true;
		const auto Snapshot = PersistenceHooksFor(FPaths::Combine(Disk.Root, TEXT("subdir")));
		if (!TestTrue(TEXT("root hook found for descendant"), Snapshot.IsValid()))
			return false;
		TestTrue(TEXT("captured hooks have an immutable configuration copy"), Snapshot.Get() != &Hooks.Hooks.Get());
		TestFalse(TEXT("other root has no hooks"), PersistenceHooksFor(Disk.Root + TEXT("other")).IsValid());
		TestTrue(TEXT("hook job admitted"), LaunchPersistence(
		                                        [Snapshot]
		                                        {
			                                        WaitPersistenceBarrier(Snapshot->BeforeRead);
			                                        FIoResult Result;
			                                        Result.bOk = Snapshot->bFailCommit;
			                                        return Result;
		                                        },
		                                        [Seen](FIoResult&& Result)
		                                        {
			                                        if (Result.bOk)
				                                        ++*Seen;
		                                        }));
		if (!TestTrue(TEXT("captured hook entered"), PumpUntil([Gate] { return Gate->Entered.load(); })))
			return false;
		// Scope releases the actual captured barrier unconditionally, including an assertion-return path.
	}
	TestTrue(TEXT("scope released worker"), Gate->Released.load());
	TestFalse(TEXT("scope unregistered root"), PersistenceHooksFor(Disk.Root).IsValid());
	TestTrue(TEXT("deferred continuation admitted"),
	         PostPersistenceContinuation(
	             [Seen] { PostPersistenceContinuation([Seen] { LaunchPersistence([] { return FIoResult(); }, [Seen](FIoResult&&) { ++*Seen; }); }); }));
	TestTrue(TEXT("drain includes deferred work that has not launched IO"), Disk.Drain());
	TestEqual(TEXT("both actual handbacks delivered"), *Seen, 2);
	TestFalse(TEXT("hook never timed out"), Gate->TimedOut.load());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestPersistenceKnownSymlinkTest,"VaRest.Http.Persistence.KnownSymlinks",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestPersistenceKnownSymlinkTest::RunTest(const FString& Parameters)
{
#if PLATFORM_LINUX
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    FScopedPersistenceTest Disk;
    struct FObserved { bool bFixture=false,bRead=false,bList=false,bWrite=false,bRootRead=false,bUnchanged=false; };
    const auto Seen=MakeShared<FObserved,ESPMode::ThreadSafe>();
    const FString Root=FPaths::ConvertRelativePathToFull(Disk.Root);
    LaunchPersistence([Root,Seen]
    {
        IPlatformFile& Platform=FPlatformFileManager::Get().GetPlatformFile();
        const FString Outside=FPaths::Combine(Root,TEXT("Outside"));
        const FString Target=FPaths::Combine(Outside,TEXT("target"));
        const FString Leaf=FPaths::Combine(Root,TEXT("leaf-link"));
        const FString DirectoryLink=FPaths::Combine(Root,TEXT("directory-link"));
        ON_SCOPE_EXIT { unlink(TCHAR_TO_UTF8(*Leaf)); unlink(TCHAR_TO_UTF8(*DirectoryLink)); };
        auto Result=WriteNewFile(Target,TArray<uint8>{1,2,3}); if (!Result.bOk) return Result;
        struct stat LeafStat,RootStat;
        Seen->bFixture=symlink(TCHAR_TO_UTF8(*Target),TCHAR_TO_UTF8(*Leaf))==0
            && symlink(TCHAR_TO_UTF8(*Outside),TCHAR_TO_UTF8(*DirectoryLink))==0
            && lstat(TCHAR_TO_UTF8(*Leaf),&LeafStat)==0 && S_ISLNK(LeafStat.st_mode)
            && lstat(TCHAR_TO_UTF8(*DirectoryLink),&RootStat)==0 && S_ISLNK(RootStat.st_mode);
        if (!Seen->bFixture) return FIoResult();
        const auto Read=ReadBoundedFile(Leaf,3);
        Seen->bRead=!Read.bOk && Read.Detail==TEXT("Symlink");
        const auto RootRead=ReadBoundedFile(FPaths::Combine(DirectoryLink,TEXT("target")),3);
        Seen->bRootRead=!RootRead.bOk && RootRead.Detail==TEXT("Symlink");
        const auto Listed=ListFiles(DirectoryLink);
        const auto Parent=ListFiles(Root);
        Seen->bList=!Listed.bOk && Listed.Detail==TEXT("Symlink") && Parent.bOk && Parent.Names.IsEmpty();
        const auto Written=WriteNewFile(FPaths::Combine(DirectoryLink,TEXT("new")),TArray<uint8>{9});
        Seen->bWrite=!Written.bOk && Written.Detail==TEXT("Symlink") && !Platform.FileExists(*FPaths::Combine(Outside,TEXT("new")));
        const auto Original=ReadBoundedFile(Target,3);
        Seen->bUnchanged=Original.bOk && Original.Bytes==TArray<uint8>({1,2,3})
            && lstat(TCHAR_TO_UTF8(*Leaf),&LeafStat)==0 && S_ISLNK(LeafStat.st_mode);
        return Original;
    },[](FIoResult&&){});
    if (!TestTrue(TEXT("real symlink worker drained"),Disk.Drain())) return false;
    TestTrue(TEXT("independent lstat fixture witness"),Seen->bFixture);
    TestTrue(TEXT("leaf read refused"),Seen->bRead);
    TestTrue(TEXT("linked parent read refused"),Seen->bRootRead);
    TestTrue(TEXT("linked root refused and leaf skipped in parent listing"),Seen->bList);
    TestTrue(TEXT("write through linked root refused"),Seen->bWrite);
    TestTrue(TEXT("outside original and link unchanged"),Seen->bUnchanged);
#endif
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestPersistencePathKindsTest,"VaRest.Http.Persistence.PathKinds",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestPersistencePathKindsTest::RunTest(const FString& Parameters)
{
#if PLATFORM_LINUX
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    FScopedPersistenceTest Disk;
    struct FSeen { bool Fixture=false,List=false,Read=false,Delete=false,Write=false,Preserved=false,Regular=false,Missing=false; };
    const auto Seen=MakeShared<FSeen,ESPMode::ThreadSafe>();
    const FString Root=FPaths::ConvertRelativePathToFull(Disk.Root);
    LaunchPersistence([Root,Seen]
    {
        FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Root);
        const FString Fifo=FPaths::Combine(Root,TEXT("unexpected-fifo"));
        int Descriptor=-1;
        ON_SCOPE_EXIT { if (Descriptor>=0) close(Descriptor); unlink(TCHAR_TO_UTF8(*Fifo)); };
        struct stat Info;
        Seen->Fixture=mkfifo(TCHAR_TO_UTF8(*Fifo),0600)==0 && lstat(TCHAR_TO_UTF8(*Fifo),&Info)==0 && S_ISFIFO(Info.st_mode);
        if (!Seen->Fixture) return FIoResult();
        // Keep a writer alive even for the historical O_RDONLY path. Size is zero,
        // so it cannot block on data; this fixture does not prove old no-writer safety.
        Descriptor=open(TCHAR_TO_UTF8(*Fifo),O_RDWR|O_NONBLOCK|O_CLOEXEC);
        Seen->Fixture=Descriptor>=0; if (!Seen->Fixture) return FIoResult();
        const auto Listed=ListFiles(Root);
        Seen->List=Listed.bOk && Listed.Names.IsEmpty();
        Seen->Read=!ReadBoundedFile(Fifo,1024).bOk;
        Seen->Delete=!VaRest::Http::DeleteFile(Fifo).bOk;
        Seen->Write=!WriteNewFile(Fifo,TArray<uint8>{9}).bOk;
        Seen->Preserved=lstat(TCHAR_TO_UTF8(*Fifo),&Info)==0 && S_ISFIFO(Info.st_mode);
        const FString Regular=FPaths::Combine(Root,TEXT("regular"));
        const auto Written=WriteNewFile(Regular,TArray<uint8>{1,2,3});
        const auto Read=ReadBoundedFile(Regular,3);
        Seen->Regular=Written.bOk && Read.bOk && Read.Bytes==TArray<uint8>({1,2,3}) && VaRest::Http::DeleteFile(Regular).bOk;
        const auto Missing=ListFiles(FPaths::Combine(Root,TEXT("missing-root")));
        Seen->Missing=Missing.bOk && Missing.Names.IsEmpty() && VaRest::Http::DeleteFile(Regular).bOk && !ReadBoundedFile(Regular,3).bOk;
        return FIoResult();
    },[](FIoResult&&){});
    if (!TestTrue(TEXT("bounded special-file worker drained"),Disk.Drain())) return false;
    TestTrue(TEXT("native FIFO and held nonblocking descriptor"),Seen->Fixture);
    TestTrue(TEXT("list admits only regular files"),Seen->List);
    TestTrue(TEXT("direct bounded read refuses special file"),Seen->Read);
    TestTrue(TEXT("delete refuses special file"),Seen->Delete);
    TestTrue(TEXT("write refuses special destination"),Seen->Write);
    TestTrue(TEXT("special node preserved"),Seen->Preserved);
    TestTrue(TEXT("ordinary file still works after special node"),Seen->Regular);
    TestTrue(TEXT("positive missing root file and idempotent delete"),Seen->Missing);
#endif
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestPersistencePathErrorsTest,"VaRest.Http.Persistence.PathErrors",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestPersistencePathErrorsTest::RunTest(const FString& Parameters)
{
#if PLATFORM_LINUX
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    FScopedPersistenceTest Disk;
    struct FSeen { bool Fixture=false,RootList=false,RootDelete=false,LeafList=false,LeafDelete=false,Restored=false; };
    const auto Seen=MakeShared<FSeen,ESPMode::ThreadSafe>();
    const FString Parent=FPaths::Combine(FPaths::ConvertRelativePathToFull(Disk.Root),TEXT("parent"));
    const FString Root=FPaths::Combine(Parent,TEXT("namespace"));
    LaunchPersistence([Parent,Root,Seen]
    {
        const FString File=FPaths::Combine(Root,TEXT("regular"));
        ON_SCOPE_EXIT { chmod(TCHAR_TO_UTF8(*Parent),0700); chmod(TCHAR_TO_UTF8(*Root),0700); };
        if (!WriteNewFile(File,TArray<uint8>{1,2,3}).bOk) return FIoResult();
        struct stat Info;
        bool Witness=geteuid()!=0 && chmod(TCHAR_TO_UTF8(*Parent),0400)==0;
        errno=0; Witness&=lstat(TCHAR_TO_UTF8(*Root),&Info)==-1 && errno==EACCES;
        Seen->RootList=!ListFiles(Root).bOk;
        Seen->RootDelete=!VaRest::Http::DeleteFile(File).bOk;
        Witness&=chmod(TCHAR_TO_UTF8(*Parent),0700)==0 && chmod(TCHAR_TO_UTF8(*Root),0400)==0;
        errno=0; Witness&=lstat(TCHAR_TO_UTF8(*File),&Info)==-1 && errno==EACCES;
        Seen->LeafList=!ListFiles(Root).bOk;
        Seen->LeafDelete=!VaRest::Http::DeleteFile(File).bOk;
        Seen->Fixture=Witness;
        chmod(TCHAR_TO_UTF8(*Root),0700);
        const auto Read=ReadBoundedFile(File,3);
        Seen->Restored=Read.bOk && Read.Bytes==TArray<uint8>({1,2,3}) && VaRest::Http::DeleteFile(File).bOk && VaRest::Http::DeleteFile(File).bOk;
        return FIoResult();
    },[](FIoResult&&){});
    if (!TestTrue(TEXT("path error worker drained and modes restored"),Disk.Drain())) return false;
    TestTrue(TEXT("actual unprivileged root and child EACCES witnesses"),Seen->Fixture);
    TestTrue(TEXT("inaccessible root is failed listing"),Seen->RootList);
    TestTrue(TEXT("inaccessible root does not acknowledge deletion"),Seen->RootDelete);
    TestTrue(TEXT("inaccessible enumerated child is failed listing"),Seen->LeafList);
    TestTrue(TEXT("inaccessible child does not acknowledge deletion"),Seen->LeafDelete);
    TestTrue(TEXT("restored ordinary file and absent deletion work"),Seen->Restored);
#endif
    return true;
}
#endif
