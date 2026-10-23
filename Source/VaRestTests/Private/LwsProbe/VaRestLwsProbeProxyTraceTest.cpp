// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestProxyInputTest, "Probe.VaRest.Lws.Rig.ProxyErrorInput",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestProxyInputTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using VaRest::LwsProbe::FEvent;
	FEvent NullInput, EmptyInput, BinaryInput, LongInput;
	const uint8 Empty = 0;
	const uint8 Binary[] = { 'x', 0, 'y' };
	TArray<uint8> LongBytes;
	LongBytes.Init('a', 300);
	CaptureErrorInput(NullInput, nullptr, 0);
	CaptureErrorInput(EmptyInput, &Empty, 0);
	CaptureErrorInput(BinaryInput, Binary, sizeof(Binary));
	CaptureErrorInput(LongInput, LongBytes.GetData(), LongBytes.Num());
	TestFalse(TEXT("null is absent"), NullInput.bInputPresent);
	TestTrue(TEXT("present empty stays present"), EmptyInput.bInputPresent);
	TestEqual(TEXT("empty has zero length"), EmptyInput.Len, int64(0));
	TestTrue(TEXT("embedded zero survives"), BinaryInput.Bytes == TArray<uint8>({ 'x', 0, 'y' }));
	TestEqual(TEXT("raw length survives cap"), LongInput.Len, int64(300));
	TestEqual(TEXT("kept bytes capped"), LongInput.Bytes.Num(), 256);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestProxyIdentityTest, "Probe.VaRest.Lws.Rig.ProxyIdentity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestProxyIdentityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	FTraceIdentity Identities;
	const void* Pointer = reinterpret_cast<const void*>(UPTRINT(1)); // never dereferenced
	const auto First = Identities.Observe(Pointer, 7, false);
	const auto Again = Identities.Observe(Pointer, 0, false);
	const auto End = Identities.Observe(Pointer, 0, true);
	const auto Reused = Identities.Observe(Pointer, 8, false);
	TestTrue(TEXT("token is nonzero"), First.Id > 0);
	TestEqual(TEXT("repeat create retains token"), Again.Id, First.Id);
	TestEqual(TEXT("destroy retains token"), End.Id, First.Id);
	TestEqual(TEXT("unresolved callbacks inherit connection"), Again.Conn, 7);
	TestTrue(TEXT("reused address is a new lifetime"), Reused.Id != First.Id);
	TestEqual(TEXT("new lifetime has new owner"), Reused.Conn, 8);
	TestEqual(TEXT("null has no token"), Identities.Observe(nullptr, 0, false).Id, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestProxyTraceLimitTest, "Probe.VaRest.Lws.Rig.ProxyTraceLimit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestProxyTraceLimitTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	FRigOptions Options;
	Options.MaxRecordedEvents = 128;
	FRig Rig(FServer(), MoveTemp(Options));
	if (!TestTrue(TEXT("context exists"), Rig.IsReady())) { return false; }
	for (int32 Index = 0; Index < 130; ++Index) { Rig.Record(0, Marker, Index, TEXT("bounded")); }
	Rig.Shutdown();
	const auto Seen = Rig.Events();
	TestEqual(TEXT("record cap"), Seen.Num(), 128);
	TestTrue(TEXT("loss is explicit"), Rig.DroppedEventCount() >= 2);
	for (int32 Index = 0; Index < Seen.Num(); ++Index)
	{
		TestEqual(TEXT("global ordered sequence"), Seen[Index].Sequence, int64(Index + 1));
	}
	return true;
}
#endif
