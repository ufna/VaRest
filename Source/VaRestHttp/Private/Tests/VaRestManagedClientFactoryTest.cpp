// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestNativeScope.h"

namespace VaRest::Http::Testing::ManagedFactoryPrivate
{
class FCreationObserver final : public IVaRestTraceSink
{
public:
	FCreationObserver(FNativeLifetimeControl* InControl, uint64 InScope, bool bInClose)
		: Control(InControl)
		, Scope(InScope)
		, bClose(bInClose)
	{
	}
	virtual FVaRestTraceDetailSettings GetDetail() const override
	{
		FVaRestTraceDetailSettings Settings;
		Settings.Flags = EVaRestTraceDetail::Metadata;
		return Settings;
	}
	virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
	virtual void OnEvent(const FVaRestTraceEvent& Event) override
	{
		if (Event.Kind != EVaRestTraceKind::Client)
			return;
		const auto& Row = Event.Payload.Get<FVaRestTraceClient>();
		if (!Row.bCreated || Row.ScopeId != Scope)
			return;
		++Created;
		bool bFound = false;
		FClientState::ForEachLive([&](FClientState& State) {
			if (State.GetClientId() != Row.ClientId)
				return;
			bFound = true;
			const auto& Managed = State.GetManagedLifetime();
			bEveryInitialOwned &= Managed.IsValid() && Managed.Control.Get() == Control && Managed.Gate.IsValid() &&
								  Managed.Operations && Managed.Client.Generation != 0 && State.GetScopeId() == Scope;
			if (bClose && Managed.IsValid())
			{
				bReentrantClose = Control->ClientSlotsOnGT().BeginRetirement(Managed);
				bClosedInsideCreated = Managed.Gate->IsClosed();
			}
		});
		bEveryInitialOwned &= bFound;
	}
	int32 Created = 0;
	bool bEveryInitialOwned = true;
	bool bReentrantClose = false;
	bool bClosedInsideCreated = false;

private:
	FNativeLifetimeControl* Control;
	uint64 Scope;
	bool bClose;
};

class FObservedCreation final
{
public:
	FObservedCreation(FNativeLifetimeControl* Control, uint64 Scope, bool bClose = false)
		: Sink(MakeShared<FCreationObserver, ESPMode::ThreadSafe>(Control, Scope, bClose))
	{
		FVaRestTrace::AddSink(Sink);
	}
	~FObservedCreation() { FVaRestTrace::RemoveSink(Sink); }
	TSharedRef<FCreationObserver, ESPMode::ThreadSafe> Sink;
};
} // namespace VaRest::Http::Testing::ManagedFactoryPrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedClientPublicationTest, "VaRest.Http.Managed.FactoryBeforeCreated",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedClientPublicationTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	FTestRig Rig;
	auto Control = FNativeLifetimeControl::Create();
	auto Scope = FNativeScopeLifetime::Open({}, 0.0);
	if (!TestTrue(TEXT("fixture owns an actual native scope"), Scope.IsValid()))
		return false;
	ManagedFactoryPrivate::FObservedCreation Observed(&Control.Get(), Scope->GetId());
	FClientConfig Initial = *FClientAccess::State(Rig.Client)->GetConfig();
	FName Error(TEXT("sentinel"));
	FVaRestClient Client = FClientAccess::CreateManagedInControl(MoveTemp(Initial), Scope.ToSharedRef(), Control, Error);
	TestTrue(TEXT("real private managed factory creates a native client"), Client.IsValid());
	TestEqual(TEXT("successful factory clears its failure output"), Error, FName(NAME_None));
	TestEqual(TEXT("exactly one initial Created is observable"), Observed.Sink->Created, 1);
	TestTrue(TEXT("Created already observes its exact Control/gate/store/generation/scope"), Observed.Sink->bEveryInitialOwned);
	TestEqual(TEXT("public native scope is final at return"), Client.GetScopeId(), Scope->GetId());
	TestFalse(TEXT("ordinary native rig retains empty managed identity"), FClientAccess::State(Rig.Client)->GetManagedLifetime().IsValid());
	TestEqual(TEXT("ordinary native rig retains process scope zero"), Rig.Client.GetScopeId(), uint64(0));
	Client = {};
	TestEqual(TEXT("no-queue factory client releases its real native slot on GT"), Control->ClientSlotsOnGT().NumOccupied(), uint32(0));
	Control->StopInputsOnGT();
	TestFalse(TEXT("completed construction needs no exit retention"), Control->IsExitRetentionRequested());
	Scope->Close(0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedFactoryCapacityTest, "VaRest.Http.Managed.FactoryCapacityBeforePublication",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedFactoryCapacityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	FTestRig Rig;
	auto Control = FNativeLifetimeControl::Create();
	auto Scope = FNativeScopeLifetime::Open({}, 0.0);
	if (!TestTrue(TEXT("fixture owns native scope"), Scope.IsValid()))
		return false;
	ManagedFactoryPrivate::FObservedCreation Observed(&Control.Get(), Scope->GetId());
	TArray<FVaRestClient> Clients;
	FName Error;
	for (uint32 Index = 0; Index < FNativeClientSlots::MaxPerScope; ++Index)
	{
		FClientConfig Initial = *FClientAccess::State(Rig.Client)->GetConfig();
		FVaRestClient Client = FClientAccess::CreateManagedInControl(MoveTemp(Initial), Scope.ToSharedRef(), Control, Error);
		if (!TestTrue(TEXT("actual client fits preadmitted scope inventory"), Client.IsValid()))
			return false;
		Clients.Add(MoveTemp(Client));
	}
	FClientConfig Extra = *FClientAccess::State(Rig.Client)->GetConfig();
	const FVaRestClient Refused = FClientAccess::CreateManagedInControl(MoveTemp(Extra), Scope.ToSharedRef(), Control, Error);
	TestFalse(TEXT("65th client refuses before exposing a native handle"), Refused.IsValid());
	TestEqual(TEXT("native capacity refusal has its actual reason"), Error, FName(TEXT("LifecycleCapacity")));
	TestEqual(TEXT("refusal does not emit a Created for an unadmitted client"), Observed.Sink->Created, int32(FNativeClientSlots::MaxPerScope));
	TestTrue(TEXT("all admitted publications already carried their ownership identity"), Observed.Sink->bEveryInitialOwned);
	Clients.Reset();
	TestEqual(TEXT("real client destruction reclaims all no-queue slots"), Control->ClientSlotsOnGT().NumOccupied(), uint32(0));
	Scope->Close(0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedFactoryReentrantCloseTest, "VaRest.Http.Managed.FactoryReentrantCreatedClose",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedFactoryReentrantCloseTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	FTestRig Rig;
	auto Control = FNativeLifetimeControl::Create();
	auto Scope = FNativeScopeLifetime::Open({}, 0.0);
	if (!TestTrue(TEXT("fixture owns native scope"), Scope.IsValid()))
		return false;
	ManagedFactoryPrivate::FObservedCreation Observed(&Control.Get(), Scope->GetId(), true);
	FClientConfig Initial = *FClientAccess::State(Rig.Client)->GetConfig();
	FName Error;
	auto Client = FClientAccess::CreateManagedInControl(MoveTemp(Initial), Scope.ToSharedRef(), Control, Error);
	if (!TestTrue(TEXT("construction returns an inert native alias after reentrant retirement"), Client.IsValid()))
		return false;
	TestTrue(TEXT("actual Created callback found the already installed native ownership"), Observed.Sink->bEveryInitialOwned);
	TestTrue(TEXT("Created callback actually closed its matching native slot"), Observed.Sink->bReentrantClose && Observed.Sink->bClosedInsideCreated);
	TestTrue(TEXT("commit never reopens a gate closed during publication"), FClientAccess::State(Client)->GetManagedLifetime().Gate->IsClosed());
	Client = {};
	TestEqual(TEXT("inert alias disposal completes real no-queue construction cleanup"), Control->ClientSlotsOnGT().NumOccupied(), uint32(0));
	Scope->Close(0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedFactoryStoppedTest, "VaRest.Http.Managed.FactoryStoppedBeforePublication",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedFactoryStoppedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	FTestRig Rig;
	auto Control = FNativeLifetimeControl::Create();
	auto Scope = FNativeScopeLifetime::Open({}, 0.0);
	if (!TestTrue(TEXT("fixture owns native scope"), Scope.IsValid()))
		return false;
	ManagedFactoryPrivate::FObservedCreation Observed(&Control.Get(), Scope->GetId());
	Control->StopInputsOnGT();
	FClientConfig Initial = *FClientAccess::State(Rig.Client)->GetConfig();
	FName Error;
	const auto Client = FClientAccess::CreateManagedInControl(MoveTemp(Initial), Scope.ToSharedRef(), Control, Error);
	TestFalse(TEXT("stopped captured Control refuses native construction"), Client.IsValid());
	TestEqual(TEXT("stopped factory reports closed lifetime"), Error, FName(TEXT("LifecycleClosed")));
	TestEqual(TEXT("stopped refusal never publishes Created"), Observed.Sink->Created, 0);
	TestEqual(TEXT("stopped refusal creates no occupied row"), Control->ClientSlotsOnGT().NumOccupied(), uint32(0));
	Scope->Close(0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedFactoryClosedScopeTest, "VaRest.Http.Managed.FactoryClosedScopeBeforePublication",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedFactoryClosedScopeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	FTestRig Rig;
	auto Control = FNativeLifetimeControl::Create();
	auto Scope = FNativeScopeLifetime::Open({}, 0.0);
	if (!TestTrue(TEXT("fixture owns native scope"), Scope.IsValid()))
		return false;
	ManagedFactoryPrivate::FObservedCreation Observed(&Control.Get(), Scope->GetId());
	Scope->Close(0.0); // this scope has no client/operation/native obligations
	TestFalse(TEXT("closed scope refusal is independent of a stopped module"), Control->IsStopped());
	FClientConfig Initial = *FClientAccess::State(Rig.Client)->GetConfig();
	FName Error;
	const auto Client = FClientAccess::CreateManagedInControl(MoveTemp(Initial), Scope.ToSharedRef(), Control, Error);
	TestFalse(TEXT("late construction in a closed native scope returns no client"), Client.IsValid());
	TestEqual(TEXT("closed scope returns the actual lifetime reason"), Error, FName(TEXT("LifecycleClosed")));
	TestEqual(TEXT("closed scope publishes no late Created"), Observed.Sink->Created, 0);
	TestEqual(TEXT("closed scope reserves no new cleanup row"), Control->ClientSlotsOnGT().NumOccupied(), uint32(0));
	return true;
}

#endif
