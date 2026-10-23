// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// The default transport's engine-exit hook (Task 7 review M-3), run on a slot of the test's own: the process's slot is shared
// with other tests (Connect.Default), and its exit cannot be undone

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestHttpTesting.h"
#include "VaRestLwsTransport.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsTransportExitTest, "VaRest.WebSocket.Lws.Transport.ExitBeforeFirstUse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsTransportExitTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	// The type GetDefaultTransport and ShutdownDefault use (VaRestLwsTransport.cpp), with the default's options
	FDefaultTransportSlot Slot{ FServiceOptions() };
	ON_SCOPE_EXIT
	{
		// Only when the exit hook let a context start (the bug): its thread stops before the slot goes
		Slot.Get()->Shutdown();
	};
	// OnEnginePreExit before anything used the transport: a modular build makes nothing at startup (plan decision 8)
	Slot.ShutdownForExit();
	const TSharedRef<FTransport, ESPMode::ThreadSafe> First = Slot.Get();
	// A request the transport refuses by itself (BadUrl, from FConn::Start), so no outcome needs the network: a transport
	// that is shut down answers Shutdown before it looks at the request
	FVaRestSocketConnectRequest Req;
	Req.Url = TEXT("ftp://127.0.0.1/");
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection = First->Connect(MoveTemp(Req));
	TestTrue(TEXT("the first connect after the exit hook ends"), VaRest::Http::Testing::PumpUntil([&Connection] { return Connection->HasEnded(); }, 5.0, true));
	TArray<FVaRestSocketEvent> Events;
	Connection->PopEvents(Events);
	TestTrue(FString::Printf(TEXT("... with Shutdown (%d event(s), the first %s, failure %s)"), Events.Num(), Events.Num() > 0 ? LexToString(Events[0].Type) : TEXT("none"),
		Events.Num() > 0 ? LexToString(Events[0].End.Failure) : TEXT("none")),
		Events.Num() == 1 && Events[0].Type == EVaRestSocketEventType::Error && Events[0].End.Failure == EVaRestSocketFailure::Shutdown);
	// Task 6's counters: no service means no lws_create_context call
	const FServiceCounters* Counters = First->GetCounters();
	TestEqual(TEXT("no context was made during exit (FServiceCounters::ContextCreations)"), Counters ? Counters->ContextCreations.load() : 0, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
