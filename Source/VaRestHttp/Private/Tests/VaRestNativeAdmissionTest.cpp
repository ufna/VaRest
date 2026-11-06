// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "Misc/AutomationTest.h"
#include "VaRestNativeLifetime.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedAdmissionTest, "VaRest.Http.Managed.AdmissionOwnership",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedAdmissionTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	auto Gate = MakeShared<FClientAdmissionGate, ESPMode::ThreadSafe>();
	FClientWorkLease Work;
	if (!TestTrue(TEXT("live managed gate admits one operation"), Gate->TryAcquire(Work)))
		return false;
	FClientWorkLease Moved = MoveTemp(Work);
	TestFalse(TEXT("moving transfers rather than duplicates the ownership"), Work.IsHeld());
	TestEqual(TEXT("transferred work remains counted"), Gate->NumOwnedOperations(), uint32(1));
	Gate->ClosePermanently();
	Gate->ClosePermanently();
	FClientWorkLease Refused;
	TestFalse(TEXT("old shared gate cannot admit after permanent close"), Gate->TryAcquire(Refused));
	TestFalse(TEXT("refusal acquires no ownership"), Refused.IsHeld());
	TestEqual(TEXT("close does not fake settlement of admitted work"), Gate->NumOwnedOperations(), uint32(1));
	// Real worker destruction of the native lease must need no GT callback, tick, UObject, or module lookup.
	auto Released = Async(EAsyncExecution::ThreadPool, [Lease = MoveTemp(Moved)]() mutable {
		Lease.Release();
		Lease.Release();
	});
	Released.Wait();
	TestEqual(TEXT("worker release settles exactly once"), Gate->NumOwnedOperations(), uint32(0));
	TestTrue(TEXT("settling the final operation cannot reopen the gate"), Gate->IsClosed());
	TestFalse(TEXT("closed zero remains a refusal"), Gate->TryAcquire(Refused));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedAdmissionCapacityTest, "VaRest.Http.Managed.AdmissionCapacity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedAdmissionCapacityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	auto Gate = MakeShared<FClientAdmissionGate, ESPMode::ThreadSafe>();
	TArray<FClientWorkLease> Work;
	Work.SetNum(FClientAdmissionGate::MaxOwnedOperations);
	for (auto& Lease : Work)
		if (!TestTrue(TEXT("each operation up to capacity is owned"), Gate->TryAcquire(Lease)))
			return false;
	FClientWorkLease Refused;
	TestFalse(TEXT("capacity refusal has no extra counted admission"), Gate->TryAcquire(Refused));
	TestEqual(TEXT("capacity refusal preserves exact live count"), Gate->NumOwnedOperations(), FClientAdmissionGate::MaxOwnedOperations);
	Work[0].Release();
	TestTrue(TEXT("settled capacity can be reused while still open"), Gate->TryAcquire(Refused));
	TestFalse(TEXT("an occupied output lease cannot hide an additional admission"), Gate->TryAcquire(Refused));
	Gate->ClosePermanently();
	Work.Empty();
	Refused.Release();
	TestEqual(TEXT("all actual owners must settle after close"), Gate->NumOwnedOperations(), uint32(0));
	TestTrue(TEXT("capacity reuse never replaces the shared close decision"), Gate->IsClosed());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedCombinedSettlementTest, "VaRest.Http.Managed.CombinedSettlement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedCombinedSettlementTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	auto Gate = MakeShared<FClientAdmissionGate, ESPMode::ThreadSafe>();
	FClientWorkLease Work;
	if (!TestTrue(TEXT("operation has a real counted admission"), Gate->TryAcquire(Work)))
		return false;
	FManagedOperationSettlement Settlement(MoveTemp(Work));
	TestTrue(TEXT("optional native handback is counted before publication"), Settlement.TryAddLogicalTail());
	TestTrue(TEXT("physical dispatch has independent settlement"), Settlement.TryAddDispatchSettlement());
	Gate->ClosePermanently();
	Settlement.CloseChildAdmission();
	Settlement.MarkLogicalTerminalSettled();
	TestFalse(TEXT("terminal closes further logical admission"), Settlement.TryAddLogicalTail());
	TestFalse(TEXT("terminal closes further dispatch admission"), Settlement.TryAddDispatchSettlement());
	Settlement.SettleLogicalTail(); // completion stack returned; the separately counted handback is still owned
	TestEqual(TEXT("logical Completed alone cannot release work"), Gate->NumOwnedOperations(), uint32(1));
	auto Logical = Async(EAsyncExecution::ThreadPool, [&Settlement] { Settlement.SettleLogicalTail(); });
	Logical.Wait();
	TestEqual(TEXT("even the last logical handback cannot fake a physical completion"), Gate->NumOwnedOperations(), uint32(1));
	TestFalse(TEXT("physical ownership keeps release unclaimed"), Settlement.WasWorkReleaseClaimed());
	auto Physical = Async(EAsyncExecution::ThreadPool, [&Settlement] { Settlement.SettleDispatch(); });
	Physical.Wait();
	TestEqual(TEXT("only combined settlement releases the admitted operation"), Gate->NumOwnedOperations(), uint32(0));
	TestTrue(TEXT("combined release was claimed once"), Settlement.WasWorkReleaseClaimed());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestManagedTailCapacityTest, "VaRest.Http.Managed.TailCapacityAndStop",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestManagedTailCapacityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	auto Gate = MakeShared<FClientAdmissionGate, ESPMode::ThreadSafe>();
	FClientWorkLease Work;
	if (!TestTrue(TEXT("operation has one preadmitted record"), Gate->TryAcquire(Work)))
		return false;
	FManagedOperationSettlement Settlement(MoveTemp(Work));
	for (uint32 Index = 1; Index < FManagedOperationSettlement::MaxLogicalTails; ++Index)
		TestTrue(TEXT("logical capacity includes the reserved completion tail"), Settlement.TryAddLogicalTail());
	TestFalse(TEXT("excess optional tail is refused before publication"), Settlement.TryAddLogicalTail());
	for (uint32 Index = 0; Index < FManagedOperationSettlement::MaxDispatchSettlements; ++Index)
		TestTrue(TEXT("simultaneous physical dispatch capacity"), Settlement.TryAddDispatchSettlement());
	TestFalse(TEXT("excess physical dispatch is refused"), Settlement.TryAddDispatchSettlement());
	TestEqual(TEXT("logical refusal cannot inflate pending ownership"), Settlement.NumLogicalTails(), FManagedOperationSettlement::MaxLogicalTails);
	TestEqual(TEXT("dispatch refusal cannot inflate pending ownership"), Settlement.NumDispatchSettlements(), FManagedOperationSettlement::MaxDispatchSettlements);
	Gate->ClosePermanently();
	Settlement.MarkDiscardedAtStop(); // core-only fixture: its caller supplies the captured-Control stopped precondition
	TestTrue(TEXT("discard records no fictitious normal result"), Settlement.WasDiscardedAtStop());
	TestFalse(TEXT("stopped row cannot admit another native tail"), Settlement.TryAddLogicalTail());
	for (uint32 Index = 0; Index < FManagedOperationSettlement::MaxLogicalTails; ++Index)
		Settlement.SettleLogicalTail();
	TestEqual(TEXT("discard cannot erase outstanding physical work"), Gate->NumOwnedOperations(), uint32(1));
	for (uint32 Index = 0; Index < FManagedOperationSettlement::MaxDispatchSettlements; ++Index)
		Settlement.SettleDispatch();
	TestEqual(TEXT("actual final settlement releases discarded work"), Gate->NumOwnedOperations(), uint32(0));
	return true;
}

#endif
