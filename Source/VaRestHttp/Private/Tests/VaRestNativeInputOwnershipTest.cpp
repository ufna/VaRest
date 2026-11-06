// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "Misc/AutomationTest.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestNativeLifetime.h"
#include "VaRestRequestSpec.h"

namespace VaRest::Http::Testing::NativeInputPrivate
{
struct FGTOnlyCapture
{
	FGTOnlyCapture(std::atomic<int32>& InDestroyed, std::atomic<bool>& InWrongThread)
		: Destroyed(InDestroyed)
		, WrongThread(InWrongThread)
	{
	}
	~FGTOnlyCapture()
	{
		if (!IsInGameThread())
			WrongThread.store(true);
		Destroyed.fetch_add(1);
	}
	std::atomic<int32>& Destroyed;
	std::atomic<bool>& WrongThread;
};
} // namespace VaRest::Http::Testing::NativeInputPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeInputOwnershipTest, "VaRest.Http.Managed.ConsumedPayloadOwnership",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeInputOwnershipTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	std::atomic<int32> Destroyed{0};
	std::atomic<int32> UserCalls{0};
	std::atomic<bool> WrongThread{false};
	auto Control = FNativeLifetimeControl::Create();
	TSharedPtr<NativeInputPrivate::FGTOnlyCapture, ESPMode::ThreadSafe> Capture =
		MakeShared<NativeInputPrivate::FGTOnlyCapture, ESPMode::ThreadSafe>(Destroyed, WrongThread);
	Rig.Client.WithHeader(TEXT("X-Version"), TEXT("a"));
	auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(Rig.Client),
		[Kept = Capture, &UserCalls](const FString&) { (void)Kept; UserCalls.fetch_add(1); });
	const auto HistoricalA = FRequestSpecAccess::NativeOwnershipBuildConfig(*Payload);
	Rig.Client.WithHeader(TEXT("X-Version"), TEXT("b"));
	Capture.Reset();
	FNativeInputBorrow Delivery;
	std::atomic<bool> CallerWasCleared{false};
	auto Adopted = Async(EAsyncExecution::ThreadPool, [Control, Payload = MoveTemp(Payload), &Delivery, &CallerWasCleared]() mutable {
		auto Sender = Control->AdoptInput(Payload);
		CallerWasCleared.store(!Payload.IsValid());
		Delivery = Sender.ForkBeforePublication();
		Sender.PublishReady();
		Sender.Reset();
	});
	Adopted.Wait();
	TestTrue(TEXT("Control adoption clears the caller's unique spec"), CallerWasCleared.load());
	TestTrue(TEXT("historical build config A survives current config B"), HistoricalA.IsValid());
	TestEqual(TEXT("worker return does not destroy the arbitrary capture"), Destroyed.load(), 0);
	Delivery.Finish();
	TestFalse(TEXT("finished input cannot acquire another GT borrow"), Control->BorrowInputOnGT(Delivery).IsValid());
	TestEqual(TEXT("even finished payload waits for the actual access authority"), Control->CollectInputsOnGT(), uint32(0));
	auto Released = Async(EAsyncExecution::ThreadPool, [Borrow = MoveTemp(Delivery)]() mutable { Borrow.Reset(); });
	Released.Wait();
	TestEqual(TEXT("worker last-borrow release only changes native bookkeeping"), Destroyed.load(), 0);
	TestEqual(TEXT("GT reclaims exactly the whole consumed payload"), Control->CollectInputsOnGT(), uint32(1));
	TestEqual(TEXT("arbitrary callback capture dies once"), Destroyed.load(), 1);
	TestFalse(TEXT("capture final destruction is never on the worker"), WrongThread.load());
	TestEqual(TEXT("payload disposal cannot invoke the user callback"), UserCalls.load(), 0);
	TestFalse(TEXT("historical A is released by whole-payload GT cleanup"), HistoricalA.IsValid());
	TestEqual(TEXT("no pending payload remains"), Control->NumPendingInputs(), uint64(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeInputUnwindTest, "VaRest.Http.Managed.UnpublishedPayloadUnwind",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeInputUnwindTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	std::atomic<int32> Destroyed{0};
	std::atomic<int32> UserCalls{0};
	std::atomic<bool> WrongThread{false};
	auto Control = FNativeLifetimeControl::Create();
	TSharedPtr<NativeInputPrivate::FGTOnlyCapture, ESPMode::ThreadSafe> Capture =
		MakeShared<NativeInputPrivate::FGTOnlyCapture, ESPMode::ThreadSafe>(Destroyed, WrongThread);
	auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(Rig.Client),
		[Kept = Capture, &UserCalls](const FString&) { (void)Kept; UserCalls.fetch_add(1); });
	Capture.Reset();
	auto Unwound = Async(EAsyncExecution::ThreadPool, [Control, Payload = MoveTemp(Payload)]() mutable {
		auto Sender = Control->AdoptInput(Payload);
		// Return before Ready/task/exchange publication: sender authority records native abandonment, not a result.
	});
	Unwound.Wait();
	TestEqual(TEXT("unpublished worker unwind preserves affine ownership"), Destroyed.load(), 0);
	TestEqual(TEXT("GT still owns initialization unwind disposal"), Control->CollectInputsOnGT(), uint32(1));
	TestEqual(TEXT("unpublished payload capture is destroyed once"), Destroyed.load(), 1);
	TestFalse(TEXT("unpublished last-owner release stays on GT"), WrongThread.load());
	TestEqual(TEXT("unpublished abandonment never fabricates a callback result"), UserCalls.load(), 0);
	Control->StopInputsOnGT();
	TestFalse(TEXT("quiescent stop needs no exit retention"), Control->IsExitRetentionRequested());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeInputInventoryTest, "VaRest.Http.Managed.TransientPayloadInventory",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeInputInventoryTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	auto Control = FNativeLifetimeControl::Create();
	constexpr uint32 Count = FClientAdmissionGate::MaxOwnedOperations + 1;
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(Rig.Client), {});
		auto Sender = Control->AdoptInput(Payload);
		Sender.PublishReady();
		Sender.Finish();
	}
	TestEqual(TEXT("transient disposal inventory is separate from the admitted operation cap"), Control->NumPendingInputs(), uint64(Count));
	TestEqual(TEXT("this is disposal-core input, not a fabricated public Send/transport fixture"), Rig.Fake->NumCalls(), 0);
	const uint64 Before = Control->NumPendingInputs();
	const uint32 Reclaimed = Control->CollectInputsOnGT(32);
	TestTrue(TEXT("one owned GT scan respects its visit budget"), Control->LastInputScanVisitsForTests() <= 32);
	TestTrue(TEXT("one owned GT scan bounds actual reclamations too"), Reclaimed <= 32);
	TestEqual(TEXT("reclamation removes actual pending nodes"), Control->NumPendingInputs(), Before - Reclaimed);
	for (uint32 Pass = 0; Pass < Count && Control->NumPendingInputs() != 0; ++Pass)
		Control->CollectInputsOnGT(32);
	TestEqual(TEXT("completed nodes leave no append-only history"), Control->NumPendingInputs(), uint64(0));
	Control->StopInputsOnGT();
	TestFalse(TEXT("fully reclaimed native input uses no exit self-hold"), Control->IsExitRetentionRequested());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeInputDeferredScanTest, "VaRest.Http.Managed.DeferredPayloadScanBudget",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeInputDeferredScanTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	std::atomic<int32> Destroyed{0};
	std::atomic<bool> WrongThread{false};
	auto Control = FNativeLifetimeControl::Create();
	TSharedPtr<NativeInputPrivate::FGTOnlyCapture, ESPMode::ThreadSafe> Capture =
		MakeShared<NativeInputPrivate::FGTOnlyCapture, ESPMode::ThreadSafe>(Destroyed, WrongThread);
	TWeakPtr<NativeInputPrivate::FGTOnlyCapture, ESPMode::ThreadSafe> ReadyTarget = Capture;
	{
		auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(Rig.Client),
			[Kept = Capture](const FString&) { (void)Kept; });
		auto Sender = Control->AdoptInput(Payload);
		Sender.PublishReady();
		Sender.Finish();
	}
	Capture.Reset();
	TArray<FNativeInputBorrow> Held;
	const auto AddBlocked = [&] {
		auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(Rig.Client), {});
		auto Sender = Control->AdoptInput(Payload);
		Sender.PublishReady();
		Sender.Finish();
		Held.Add(MoveTemp(Sender)); // real finished payload remains unreclaimable through this access authority
	};
	for (int32 Index = 0; Index < 12; ++Index)
		AddBlocked(); // ready target lies behind this incoming prefix
	TestTrue(TEXT("selected ready target still has its actual payload owner"), ReadyTarget.IsValid());
	for (int32 Pass = 0; Pass < 96 && ReadyTarget.IsValid(); ++Pass)
	{
		if ((Pass % 2) == 0)
			AddBlocked(); // new arrivals cannot starve the already detached ready target
		Control->CollectInputsOnGT(4);
		TestTrue(TEXT("held/deferred nodes also consume the visit budget"), Control->LastInputScanVisitsForTests() <= 4);
	}
	TestFalse(TEXT("deferred rotation and continued arrivals cannot hide the selected ready target"), ReadyTarget.IsValid());
	TestEqual(TEXT("selected target is actually destroyed once before held nodes release"), Destroyed.load(), 1);
	TestFalse(TEXT("selected target destruction remains on GT"), WrongThread.load());
	TestEqual(TEXT("held nodes still own their real pending payloads"), Control->NumPendingInputs(), uint64(Held.Num()));
	Held.Empty();
	for (int32 Pass = 0; Pass < 128 && Control->NumPendingInputs() != 0; ++Pass)
	{
		Control->CollectInputsOnGT(16);
		TestTrue(TEXT("cleanup also respects inspected-node budget"), Control->LastInputScanVisitsForTests() <= 16);
	}
	TestEqual(TEXT("owned scan fixture releases every remaining payload"), Control->NumPendingInputs(), uint64(0));
	return true;
}

#endif
