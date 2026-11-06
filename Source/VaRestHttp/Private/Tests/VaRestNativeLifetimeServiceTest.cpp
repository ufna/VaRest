// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "VaRestNativeLifetimeService.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestRequestSpec.h"

namespace VaRest::Http::Testing::ServiceReentrancyPrivate
{
struct FResetServiceOnDestroy
{
	FResetServiceOnDestroy(TUniquePtr<FNativeLifetimeService>* InService, int32* InCalls, bool* InWrongThread)
		: Service(InService)
		, Calls(InCalls)
		, WrongThread(InWrongThread)
	{
	}
	~FResetServiceOnDestroy()
	{
		++*Calls;
		*WrongThread |= !IsInGameThread();
		Service->Reset();
	}
	TUniquePtr<FNativeLifetimeService>* Service;
	int32* Calls;
	bool* WrongThread;
};
} // namespace VaRest::Http::Testing::ServiceReentrancyPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeServiceTickTest, "VaRest.Http.Managed.OwnedLifetimeTicker",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeServiceTickTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	FTSTicker OwnedTicker;
	auto Service = FNativeLifetimeService::CreateOnGT(OwnedTicker);
	if (!TestTrue(TEXT("real native ticker service is initialized before managed publication"), Service.IsValid()))
		return false;
	auto Control = Service->ControlForNewClientOnGT();
	auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(Rig.Client), {});
	auto Sender = Control->AdoptInput(Payload);
	Sender.PublishReady();
	Sender.Finish(true);
	Sender.Reset();
	TestEqual(TEXT("completed payload remains actually owned until GT collection"), Control->NumPendingInputs(), uint64(1));
	OwnedTicker.Tick(0.0f); // real isolated ticker only; never drain unrelated game-thread tasks/core tickers
	TestEqual(TEXT("registered weak ticker invokes the actual input collector"), Control->NumPendingInputs(), uint64(0));
	Service->BeginStoppingOnGT();
	TestFalse(TEXT("one-way pre-exit phase refuses new client publication"), Service->ControlForNewClientOnGT().IsValid());
	TestFalse(TEXT("closing admission precedes publishing final Stopped"), Control->IsStopped());
	Service->StopOnGT();
	TestTrue(TEXT("final stop publishes into the captured Control"), Control->IsStopped());
	TestFalse(TEXT("empty Control requests no exit quarantine"), Control->IsExitRetentionRequested());
	OwnedTicker.Tick(0.0f); // removed callback cannot restart admission or post new cleanup work
	TestFalse(TEXT("stopped service remains closed after its old ticker's next frame"), Service->ControlForNewClientOnGT().IsValid());
	Service->StopOnGT();
	Service.Reset();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeEmptyStoppedControlTest, "VaRest.Http.Managed.EmptyStoppedControlWorkerRelease",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeEmptyStoppedControlTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("EmptyStoppedControlWorkerRelease requires an actual worker; skipped without multithreading support"));
		return true;
	}
	FTSTicker OwnedTicker;
	auto Service = FNativeLifetimeService::CreateOnGT(OwnedTicker);
	if (!TestTrue(TEXT("native service creation succeeds"), Service.IsValid()))
		return false;
	auto Captured = Service->ControlForNewClientOnGT();
	TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Weak = Captured;
	Service->StopOnGT();
	Service.Reset();
	TestTrue(TEXT("an internal captured native reference outlives the module-owned service"), Weak.IsValid());
	std::atomic<bool> RanOffGT{false};
	auto Released = Async(EAsyncExecution::ThreadPool, [Held = MoveTemp(Captured), &RanOffGT]() mutable {
		RanOffGT.store(!IsInGameThread());
		Held.Reset(); // actual empty-Control destruction, not a copied counter/predicate
	});
	Released.Wait();
	TestTrue(TEXT("final native reference actually releases on worker"), RanOffGT.load());
	TestFalse(TEXT("quiescent stopped Control is destroyed without a retained history"), Weak.IsValid());
	OwnedTicker.Tick(0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeServiceReentrancyTest, "VaRest.Http.Managed.ServiceStopOwnerReentrancy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeServiceReentrancyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	FTSTicker OwnedTicker;
	auto Service = FNativeLifetimeService::CreateOnGT(OwnedTicker);
	if (!TestTrue(TEXT("fixture owns a real native service"), Service.IsValid()))
		return false;
	auto Control = Service->ControlForNewClientOnGT();
	TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Weak = Control;
	int32 Destroyed = 0;
	bool WrongThread = false;
	TSharedPtr<ServiceReentrancyPrivate::FResetServiceOnDestroy, ESPMode::ThreadSafe> Trigger =
		MakeShared<ServiceReentrancyPrivate::FResetServiceOnDestroy, ESPMode::ThreadSafe>(&Service, &Destroyed, &WrongThread);
	auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(Rig.Client),
		[Held = MoveTemp(Trigger)](const FString&) {});
	auto Sender = Control->AdoptInput(Payload);
	Sender.PublishReady();
	Sender.Finish(true);
	Sender.Reset();
	Control.Reset();
	Service->StopOnGT(); // actual input destruction reenters/deletes the unique service owner during this method
	TestFalse(TEXT("real cleanup destroyed the service receiver before its outer Stop returned"), Service.IsValid());
	TestEqual(TEXT("actual payload capture destructor ran exactly once"), Destroyed, 1);
	TestFalse(TEXT("unique-service destruction remains on GT"), WrongThread);
	TestFalse(TEXT("outer native locals finish safely with no retained empty Control"), Weak.IsValid());
	OwnedTicker.Tick(0.0f); // actual removed ticker must not address the deleted service
	return true;
}

#endif
