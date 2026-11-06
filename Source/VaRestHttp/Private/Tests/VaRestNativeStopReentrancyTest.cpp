// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestNativeClientSlots.h"
#include "VaRestNativeScope.h"
#include "VaRestRequestSpec.h"

namespace VaRest::Http::Testing::StopReentrancyPrivate
{
struct FResetExternalOnDestroy
{
	FResetExternalOnDestroy(TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe>* InExternal, int32* InCalls, bool* InWrongThread)
		: External(InExternal)
		, Calls(InCalls)
		, WrongThread(InWrongThread)
	{
	}
	~FResetExternalOnDestroy()
	{
		++*Calls;
		*WrongThread |= !IsInGameThread();
		External->Reset();
	}
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe>* External;
	int32* Calls;
	bool* WrongThread;
};
} // namespace VaRest::Http::Testing::StopReentrancyPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeStopAnchorReentrancyTest, "VaRest.Http.Managed.StopAnchorFinalOwnerReentrancy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeStopAnchorReentrancyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Control = FNativeLifetimeControl::Create();
	TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Weak = Control;
	FNativeLifetimeControl* const Identity = Control.Get();
	auto Scope = FNativeScopeLifetime::Open({}, 0.0);
	FNativeClientReservation ClientReservation;
	FName Error;
	if (!TestTrue(TEXT("fixture reserves an actual Control-owned slot"), Control->ClientSlotsOnGT().TryReserve(Scope.ToSharedRef(), ClientReservation, Error)))
		return false;
	auto Registration = ClientReservation.InitialRegistration();
	FClientConfig Config = *FClientAccess::State(Rig.Client)->GetConfig();
	Config.ScopeLifetime = Scope;
	Config.ScopeId = Scope->GetId();
	auto Client = FClientState::Create(MoveTemp(Config));
	if (!TestTrue(TEXT("real scoped client commits its exclusive reservation"), ClientReservation.CommitClient(Client)))
		return false;
	auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(Client, {});
	auto Sender = Control->AdoptInput(Payload);
	FClientWorkLease Work;
	if (!TestTrue(TEXT("initial counted lease is real"), Registration.Gate->TryAcquire(Work)))
		return false;
	FNativeOperationReservation Operation;
	if (!TestTrue(TEXT("fixture owns a real unpublished cell"), Registration.Operations->TryClaim(Work, Sender, Operation)))
		return false;
	int32 AnchorCalls = 0;
	bool WrongThread = false;
	auto InputAnchor = Operation.ForkInputAnchor();
	Operation.SetReleaseAnchor([Input = MoveTemp(InputAnchor), &Control, &AnchorCalls, &WrongThread](bool Discarded) mutable {
		++AnchorCalls;
		WrongThread |= !IsInGameThread();
		Input.Finish(Discarded);
		Input.Reset();
		Control.Reset(); // actual last EXTERNAL receiver owner drops inside Stop's nested store-sweep callout
	});
	auto Initial = Operation.Publish();
	Registration.Operations->MarkTerminalOnGT(Initial.Reference());
	Initial.Reset();
	Registration.Control.Reset(); // no inert registration/token copy is allowed to hide the original lifetime defect
	Control->StopInputsOnGT();
	TestFalse(TEXT("anchor actually reset the external caller's shared pointer"), Control.IsValid());
	TestEqual(TEXT("real ready cell anchor ran once during Stop"), AnchorCalls, 1);
	TestFalse(TEXT("reentrant cleanup remains on GT"), WrongThread);
	auto Kept = Weak.Pin();
	if (!TestTrue(TEXT("Stop survived long enough to install the same Control's occupied-client exit hold"), Kept.IsValid()))
		return false;
	TestTrue(TEXT("retained Control is the original receiver, not a replacement owner"), Kept.Get() == Identity);
	TestTrue(TEXT("actual occupied client requests process-exit retention"), Kept->IsExitRetentionRequested());
	TestEqual(TEXT("actual input collector completed before Stop's final hold decision"), Kept->NumPendingInputs(), uint64(0));
	Registration.Control = Kept;
	TestTrue(TEXT("fixture has no non-operation obligations; final row can now quiesce"), Kept->ClientSlotsOnGT().TryReleaseQuiescent(Registration));
	TestTrue(TEXT("test-only teardown removes its now-quiescent hold without production policy changes"), Kept->DropQuiescentExitHoldForTests());
	Registration.Control.Reset();
	Kept.Reset();
	TestFalse(TEXT("repeated automation retains no fixture Control allocation"), Weak.IsValid());
	Scope->Close(0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestNativeStopInputReentrancyTest, "VaRest.Http.Managed.StopInputFinalOwnerReentrancy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestNativeStopInputReentrancyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Control = FNativeLifetimeControl::Create();
	TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Weak = Control;
	int32 Destroyed = 0;
	bool WrongThread = false;
	TSharedPtr<StopReentrancyPrivate::FResetExternalOnDestroy, ESPMode::ThreadSafe> Trigger =
		MakeShared<StopReentrancyPrivate::FResetExternalOnDestroy, ESPMode::ThreadSafe>(&Control, &Destroyed, &WrongThread);
	auto Payload = FRequestSpecAccess::MakeNativeOwnershipPayload(FClientAccess::State(Rig.Client),
		[Held = MoveTemp(Trigger)](const FString&) {});
	auto Sender = Control->AdoptInput(Payload);
	Sender.PublishReady();
	Sender.Finish(true);
	Sender.Reset();
	Control->StopInputsOnGT(); // actual whole-spec destructor drops the last external Control inside CollectInputsOnGT
	TestFalse(TEXT("whole-spec capture actually reset the external caller owner"), Control.IsValid());
	TestEqual(TEXT("real capture destructor ran once"), Destroyed, 1);
	TestFalse(TEXT("capture destruction remains GT"), WrongThread);
	TestFalse(TEXT("safe empty Stop return retains no unnecessary self-hold"), Weak.IsValid());
	return true;
}

#endif
