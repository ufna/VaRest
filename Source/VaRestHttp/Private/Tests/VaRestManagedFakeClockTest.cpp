// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestManagedDispatch.h"
#include "VaRestManagedOperation.h"
#include "VaRestNativeOwnershipFixture.h"

namespace VaRest::Http::Testing::ManagedFakeClockPrivate
{
struct FState
{
	int32 ClockDeaths = 0;
	int32 Replies = 0;
	int32 ReplyStatus = 0;
	int32 ReplyBytes = 0;
	bool WrongThread = false;
};
class FClock final : public IVaRestClock
{
public:
	FClock(TSharedRef<FState, ESPMode::ThreadSafe> InState, TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> InClock)
		: State(MoveTemp(InState))
		, Clock(MoveTemp(InClock))
	{
	}
	~FClock()
	{
		++State->ClockDeaths;
		State->WrongThread |= !IsInGameThread();
	}
	double Now() const override { return Clock->Now(); }
	FVaRestTimerHandle SetTimer(double Delay, TUniqueFunction<void()>&& Callback) override { return Clock->SetTimer(Delay, MoveTemp(Callback)); }
	void ClearTimer(FVaRestTimerHandle& Handle) override { Clock->ClearTimer(Handle); }

private:
	TSharedRef<FState, ESPMode::ThreadSafe> State;
	TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock;
};

/** Exact fresh fake record only. Runs BEFORE the native fixture closes/collects its real operation on every return. */
struct FRecordCleanup
{
	explicit FRecordCleanup(TSharedRef<FVaRestFakeTransport, ESPMode::ThreadSafe> InFake)
		: Fake(MoveTemp(InFake))
	{
		Fake->bCompleteOnWorker = false; // own fake instance; this test launches no worker or named GT task
	}
	~FRecordCleanup()
	{
		check(IsInGameThread());
		if (Fake->NumCalls() == 1)
		{
			if (!Fake->GetCall(0).bCompleted)
				Fake->Drop(0); // actual uncalled callback disposal; its payload is native test state, never a managed guard
			Fake->ReleaseCompletedClock(0);
		}
	}
	TSharedRef<FVaRestFakeTransport, ESPMode::ThreadSafe> Fake;
};
} // namespace VaRest::Http::Testing::ManagedFakeClockPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedFakeClockTest, "VaRest.Http.Managed.FakeCompletedClockRetention",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedFakeClockTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	NativeOwnership::FFixture F(true);
	ManagedFakeClockPrivate::FRecordCleanup Cleanup(F.Rig.Fake);
	if (!TestNotNull(TEXT("real Control-owned store reserved"), F.Store))
		return false;
	auto Input = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(F.Rig.Client), {});
	F.Sender = F.Control->AdoptInput(Input);
	if (!TestTrue(TEXT("actual gate admission"), F.Gate->TryAcquire(F.Work)) ||
		!TestTrue(TEXT("actual cell claim"), F.Store->TryClaim(F.Work, F.Sender, F.Claim, 2)))
		return false;
	auto* Payload = F.Claim.CreateManagedPayload();
	if (!TestNotNull(TEXT("actual typed release anchor installed"), Payload))
		return false;
	F.Handoff = F.Claim.Publish(F.Completion, F.DeliveryWorker);
	if (!TestTrue(TEXT("initial real authorities published"), F.Handoff.IsHeld()))
		return false;
	auto State = MakeShared<ManagedFakeClockPrivate::FState, ESPMode::ThreadSafe>();
	TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Original = MakeShared<ManagedFakeClockPrivate::FClock, ESPMode::ThreadSafe>(State, F.Rig.Clock);
	FVaRestTransportRequest Request;
	Request.Verb = TEXT("GET");
	Request.Url = TEXT("https://api.test/owned-clock");
	Request.Headers.Set(TEXT("X-Owned-Record"), TEXT("preserve"));
	{
		FDispatchTag Tag;
		Tag.Serial = 1;
		FVaRestOutcomeCallback Outcome = [](FVaRestAttemptOutcome&&) {};
		FManagedAttemptDelivery Delivery = [](FDispatchTag&&, FVaRestTransportResult&&, FVaRestOutcomeCallback&&) {};
		auto Prepared = Payload->DispatchesOnGT().PrepareOnGT(Tag, Outcome, Original, Delivery);
		if (!TestTrue(TEXT("real counted dispatch view prepared"), Prepared.IsValid()))
			return false;
		// Isolate the real request-record clock owner. Normal managed guard/result/GT routing is a separate integration gate.
		auto Unstarted = Prepared.TakeUnstartedOutcomeOnGT();
		Unstarted = nullptr;
		Request.Clock = Prepared.ClockViewOnGT();
	}
	Original.Reset();
	TestFalse(TEXT("invalid completed-record release refuses"), F.Rig.Fake->ReleaseCompletedClock(-1));
	F.Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	auto Call = F.Rig.Fake->Start(MoveTemp(Request), [State](FVaRestTransportResult&& Result) {
		++State->Replies;
		State->ReplyStatus = Result.Status;
		State->ReplyBytes = Result.Body.Num();
		State->WrongThread |= !IsInGameThread();
	});
	TestFalse(TEXT("an actual in-flight record cannot release its request clock"), F.Rig.Fake->ReleaseCompletedClock(0));
	F.Store->MarkTerminalOnGT(F.Handoff.Reference()); // ownership fixture terminal, NOT a fabricated public HTTP completion
	F.Completion.Reset();
	F.Handoff.Reset();
	TestEqual(TEXT("real fake request clock alone retains physical settlement"), F.Gate->NumOwnedOperations(), uint32(1));
	F.Rig.Fake->Complete(0, FVaRestFakeResponse::Text(218, TEXTVIEW("payload")));
	TestEqual(TEXT("actual fake callback returned"), State->Replies, 1);
	TestEqual(TEXT("actual result status passed through fake transport"), State->ReplyStatus, 218);
	TestEqual(TEXT("actual result body passed through fake transport"), State->ReplyBytes, 7);
	TestTrue(TEXT("completed call record still retains its actual request clock"), F.Rig.Fake->GetCall(0).Request.Clock.IsValid());
	TestEqual(TEXT("callback return is not actual clock-view settlement"), F.Gate->NumOwnedOperations(), uint32(1));
	TestEqual(TEXT("actual original remains in GT payload"), State->ClockDeaths, 0);
	TestTrue(TEXT("explicit completed-record clock release succeeds"), F.Rig.Fake->ReleaseCompletedClock(0));
	TestFalse(TEXT("only the completed record's clock reference was removed"), F.Rig.Fake->GetCall(0).Request.Clock.IsValid());
	TestEqual(TEXT("request URL history retained"), F.Rig.Fake->GetCall(0).Request.Url, FString(TEXT("https://api.test/owned-clock")));
	TestEqual(TEXT("request header history retained"), F.Rig.Fake->GetCall(0).Request.Headers.Get(TEXT("X-Owned-Record")), FString(TEXT("preserve")));
	TestEqual(TEXT("actual last view drop settles its physical authority"), F.Gate->NumOwnedOperations(), uint32(0));
	TestEqual(TEXT("settlement alone does not destroy original clock outside owned sweep"), State->ClockDeaths, 0);
	F.Store->SweepOnGT(FNativeOperationStore::Capacity);
	F.Control->CollectInputsOnGT();
	TestEqual(TEXT("owned GT sweep destroys original exactly once"), State->ClockDeaths, 1);
	TestFalse(TEXT("callback and actual original destructor ran on GT"), State->WrongThread);
	TestEqual(TEXT("actual cell reclaimed after explicit view release"), F.Store->NumOccupied(), uint32(0));
	TestEqual(TEXT("actual residual input reclaimed"), F.Control->NumPendingInputs(), uint64(0));
	return true;
}

#endif
