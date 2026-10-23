// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "VaRestHttpTesting.h"
#include "VaRestLwsService.h"

#include <atomic>

namespace VaRest::WebSocket::Lws::VaRestLwsServiceTestPrivate
{
	/** What a stand-in connection saw; shared with the service thread */
	struct FStandInLog
	{
		std::atomic<bool> bStarted{ false };
		std::atomic<int32> Checks{ 0 };
		std::atomic<double> FiredLate{ -1.0 };
		std::atomic<int32> ShutdownEnds{ 0 };
	};

	/** A connection with one deadline and no wsi: the service's loop is all it exercises */
	class FStandIn final : public IServiceConnection
	{
	public:
		FStandIn(uint64 InId, double InDeadline, bool bInShort, TSharedRef<FStandInLog, ESPMode::ThreadSafe> InLog)
			: Id(InId), Deadline(InDeadline), bShort(bInShort), Log(MoveTemp(InLog))
		{
		}

		virtual uint64 GetId() const override { return Id; }
		virtual void Start(FService& Service) override { Log->bStarted = true; }
		virtual int32 OnCallback(FService& Service, lws* InWsi, int32 Reason, void* User, void* In, size_t Len) override { return 0; }
		virtual void CheckDeadlines(FService& Service, double Now) override
		{
			++Log->Checks;
			if (!bFired && Now >= Deadline)
			{
				bFired = true;
				Log->FiredLate = Now - Deadline;
				if (bWritesCloseOnFire)
				{
					// Stands in for FConn's close write: the close it was asked for has gone out
					Service.GetCounters().UnsentCloses.fetch_sub(1);
				}
			}
		}
		virtual double NextDeadline() const override { return bFired ? TNumericLimits<double>::Max() : Deadline; }
		virtual bool WantsShortWait() const override { return bShort; }
		virtual bool IsDestroyed() const override { return bFired && bEndOnFire; }
		virtual void EndForShutdown() override { ++Log->ShutdownEnds; }

		bool bEndOnFire = true;
		/** The deadline writes a close FlushCloses waits for (review conc M-3) */
		bool bWritesCloseOnFire = false;

	private:
		const uint64 Id;
		const double Deadline;
		const bool bShort;
		bool bFired = false;
		TSharedRef<FStandInLog, ESPMode::ThreadSafe> Log;
	};

	struct FRan
	{
		FCriticalSection Lock;
		TArray<int32> Order;
		TSet<uint32> Threads;
		std::atomic<int32> Count{ 0 };
		double FirstLatency = -1.0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceStartStopTest, "VaRest.WebSocket.Lws.Service.StartStop",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceStartStopTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	const TSharedRef<FService, ESPMode::ThreadSafe> Service = MakeShared<FService, ESPMode::ThreadSafe>(FServiceOptions());
	if (!TestTrue(TEXT("the service starts"), Service->Start()))
	{
		return false;
	}
	TestTrue(TEXT("it runs"), Service->IsRunning());
	TestNotNull(TEXT("with a context"), Service->GetContext());
	TestNotNull(TEXT("and the engine's certificate manager"), Service->GetCertificates());
	Service->Shutdown();
	TestFalse(TEXT("it stops"), Service->IsRunning());
	TestNull(TEXT("its context is destroyed"), Service->GetContext());
	Service->Shutdown();
	TestFalse(TEXT("a second Shutdown changes nothing, and a shut-down service never starts again"), Service->Start());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceCommandsTest, "VaRest.WebSocket.Lws.Service.Commands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceCommandsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	using namespace VaRest::WebSocket::Lws::VaRestLwsServiceTestPrivate;
	AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	FServiceOptions Options;
	// No connection, so the service waits this long unless lws_cancel_service wakes it (T13, svc.cancel_wake_ms)
	Options.IdleWaitMs = 30000;
	const TSharedRef<FService, ESPMode::ThreadSafe> Service = MakeShared<FService, ESPMode::ThreadSafe>(MoveTemp(Options));
	if (!TestTrue(TEXT("the service starts"), Service->Start()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Service->Shutdown();
	};
	// lws_create_context ends with its own lws_cancel_service (the Release archive: context.c.o 0x161d-0x1662 is the body of
	// lws_cancel_service, 0xef0-0xf39, inlined), so a new context's first lws_service drains whatever is queued by then, woken
	// or not. A first command, run, has taken that wake: the 100 below are queued while the thread waits its 30 s
	const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Warm = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	Service->Enqueue([Warm](FService& Self) { *Warm = true; });
	if (!TestTrue(TEXT("a first command ran"), VaRest::Http::Testing::PumpUntil([Warm] { return Warm->load(); }, 5.0)))
	{
		return false;
	}
	const TSharedRef<FRan, ESPMode::ThreadSafe> Ran = MakeShared<FRan, ESPMode::ThreadSafe>();
	const double Sent = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < 100; ++Index)
	{
		Service->Enqueue([Ran, Index, Sent](FService& Self)
		{
			FScopeLock Guard(&Ran->Lock);
			if (Index == 0)
			{
				Ran->FirstLatency = FPlatformTime::Seconds() - Sent;
			}
			Ran->Order.Add(Index);
			Ran->Threads.Add(FPlatformTLS::GetCurrentThreadId());
			++Ran->Count;
		});
	}
	TestTrue(TEXT("every command ran"), VaRest::Http::Testing::PumpUntil([Ran] { return Ran->Count.load() == 100; }, 5.0));
	FScopeLock Held(&Ran->Lock);
	bool bInOrder = Ran->Order.Num() == 100;
	for (int32 Index = 0; bInOrder && Index < 100; ++Index)
	{
		bInOrder = Ran->Order[Index] == Index;
	}
	TestTrue(TEXT("in the order they were queued"), bInOrder);
	TestEqual(TEXT("on one thread"), Ran->Threads.Num(), 1);
	TestFalse(TEXT("which is not the game thread"), Ran->Threads.Contains(GGameThreadId));
	TestTrue(FString::Printf(TEXT("woken at once, not after the 30 s wait (%.2f ms)"), Ran->FirstLatency * 1000.0), Ran->FirstLatency >= 0.0 && Ran->FirstLatency < 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceWaitTest, "VaRest.WebSocket.Lws.Service.Wait",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceWaitTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	using namespace VaRest::WebSocket::Lws::VaRestLwsServiceTestPrivate;
	// Not started: the game thread owns its state, so ComputeWaitMs can be read directly
	FService Service{ FServiceOptions() };
	TestEqual(TEXT("no connection: the idle wait"), Service.ComputeWaitMs(100.0), 30000);
	const TSharedRef<FStandInLog, ESPMode::ThreadSafe> Log = MakeShared<FStandInLog, ESPMode::ThreadSafe>();
	Service.Adopt(MakeUnique<FStandIn>(1, 100.25, false, Log));
	TestTrue(TEXT("Adopt starts the connection"), Log->bStarted.load());
	TestEqual(TEXT("a deadline 250 ms ahead: 250 ms"), Service.ComputeWaitMs(100.0), 250);
	TestEqual(TEXT("one 5 s ahead: the 1 s cap (spec 2.1)"), Service.ComputeWaitMs(95.25), 1000);
	TestEqual(TEXT("a deadline passed: no wait"), Service.ComputeWaitMs(101.0), 0);
	Service.Adopt(MakeUnique<FStandIn>(2, TNumericLimits<double>::Max(), true, Log));
	TestEqual(TEXT("a kill in flight: the 100 ms cap (plan decision 12)"), Service.ComputeWaitMs(95.25), 100);
	TestNotNull(TEXT("Find by id"), Service.Find(2));
	TestNull(TEXT("... and only those"), Service.Find(3));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceDeadlinesTest, "VaRest.WebSocket.Lws.Service.Deadlines",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceDeadlinesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	using namespace VaRest::WebSocket::Lws::VaRestLwsServiceTestPrivate;
	AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	const TSharedRef<FService, ESPMode::ThreadSafe> Service = MakeShared<FService, ESPMode::ThreadSafe>(FServiceOptions());
	if (!TestTrue(TEXT("the service starts"), Service->Start()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Service->Shutdown();
	};
	const TSharedRef<FStandInLog, ESPMode::ThreadSafe> Log = MakeShared<FStandInLog, ESPMode::ThreadSafe>();
	const double Deadline = FPlatformTime::Seconds() + 0.3;
	Service->Enqueue([Log, Deadline](FService& Self) { Self.Adopt(MakeUnique<FStandIn>(7, Deadline, false, Log)); });
	TestTrue(TEXT("the deadline is checked after lws_service returns, on time"), VaRest::Http::Testing::PumpUntil([Log] { return Log->FiredLate.load() >= 0.0; }, 5.0));
	// 250 ms, not 100: the service thread runs below normal priority (UnixPlatformProcess.cpp:2623-2625) on a host that may be
	// building (review conc M-8)
	TestTrue(FString::Printf(TEXT("... within 250 ms of it (%.1f ms late)"), Log->FiredLate.load() * 1000.0), Log->FiredLate.load() < 0.25);
	TestTrue(TEXT("a destroyed connection is erased"), VaRest::Http::Testing::PumpUntil([Service] { return Service->GetCounters().LiveConnections.load() == 0; }, 5.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceShutdownTest, "VaRest.WebSocket.Lws.Service.Shutdown",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceShutdownTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	using namespace VaRest::WebSocket::Lws::VaRestLwsServiceTestPrivate;
	AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	const TSharedRef<FService, ESPMode::ThreadSafe> Service = MakeShared<FService, ESPMode::ThreadSafe>(FServiceOptions());
	if (!TestTrue(TEXT("the service starts"), Service->Start()))
	{
		return false;
	}
	const TSharedRef<FStandInLog, ESPMode::ThreadSafe> Log = MakeShared<FStandInLog, ESPMode::ThreadSafe>();
	Service->Enqueue([Log](FService& Self) { Self.Adopt(MakeUnique<FStandIn>(9, TNumericLimits<double>::Max(), false, Log)); });
	TestTrue(TEXT("adopted"), VaRest::Http::Testing::PumpUntil([Log] { return Log->bStarted.load(); }, 5.0));
	Service->Shutdown();
	TestEqual(TEXT("Shutdown ends each connection once (its terminal event)"), Log->ShutdownEnds.load(), 1);
	const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Late = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	Service->Enqueue([Late](FService& Self) { *Late = true; });
	// Review conc M-7: nothing would run a queued command either, so check that it was let go at once -- its capture released
	TestEqual(TEXT("a command after Shutdown is dropped at once (its capture released)"), Late.GetSharedReferenceCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceFlushTest, "VaRest.WebSocket.Lws.Service.ShutdownFlushesCloses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceFlushTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	using namespace VaRest::WebSocket::Lws::VaRestLwsServiceTestPrivate;
	AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	const TSharedRef<FService, ESPMode::ThreadSafe> Service = MakeShared<FService, ESPMode::ThreadSafe>(FServiceOptions());
	if (!TestTrue(TEXT("the service starts"), Service->Start()))
	{
		return false;
	}
	// Review conc M-3: a close asked for just before Shutdown (the hub's 1001 at engine exit) is written 50 ms later; Shutdown,
	// called at once, waits for it (ExitFlushSeconds 0.25 by default) instead of stopping the thread first
	const TSharedRef<FStandInLog, ESPMode::ThreadSafe> Log = MakeShared<FStandInLog, ESPMode::ThreadSafe>();
	const double Deadline = FPlatformTime::Seconds() + 0.05;
	Service->Enqueue([Log, Deadline](FService& Self)
	{
		Self.GetCounters().UnsentCloses.fetch_add(1);
		TUniquePtr<FStandIn> Closing = MakeUnique<FStandIn>(11, Deadline, false, Log);
		Closing->bWritesCloseOnFire = true;
		Self.Adopt(MoveTemp(Closing));
	});
	const double Before = FPlatformTime::Seconds();
	Service->Shutdown();
	const double Took = FPlatformTime::Seconds() - Before;
	TestTrue(TEXT("the close went out before the thread stopped"), Log->FiredLate.load() >= 0.0);
	TestTrue(FString::Printf(TEXT("... and Shutdown waited no longer than ExitFlushSeconds (%.0f ms)"), Took * 1000.0), Took < 0.25 + 0.1);
	TestEqual(TEXT("nothing is left unsent"), Service->GetCounters().UnsentCloses.load(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceStartLatchTest, "VaRest.WebSocket.Lws.Service.StartFailureLatched",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceStartLatchTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	// Review conc I-2: exactly one failure, so exactly one line
	AddExpectedErrorPlain(TEXT("the service thread could not start"), EAutomationExpectedErrorFlags::Contains, 1);
	FServiceOptions Options;
	Options.bFailThreadForTest = true;
	const TSharedRef<FService, ESPMode::ThreadSafe> Service = MakeShared<FService, ESPMode::ThreadSafe>(MoveTemp(Options));
	TestFalse(TEXT("the start fails"), Service->Start());
	TestFalse(TEXT("... and so does the next, at once"), Service->Start());
	TestEqual(TEXT("one lws_create_context, never a second (VaRest-rti)"), Service->GetCounters().ContextCreations.load(), 1);
	TestNull(TEXT("no context is left"), Service->GetContext());
	Service->Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceHandshakeGateTest, "VaRest.WebSocket.Lws.Service.RefusesCreateUnderHandshake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceHandshakeGateTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	AddExpectedErrorPlain(TEXT("VaRest wss handshake(s) are in flight"), EAutomationExpectedErrorFlags::Contains, 1);
	const TSharedRef<FService, ESPMode::ThreadSafe> Service = MakeShared<FService, ESPMode::ThreadSafe>(FServiceOptions());
	{
		// Review conc I-3: a VaRest wss attempt of another service is in flight
		TlsHandshakesInFlight().fetch_add(1);
		ON_SCOPE_EXIT
		{
			TlsHandshakesInFlight().fetch_sub(1);
		};
		TestFalse(TEXT("no context is made while a wss handshake is in flight"), Service->Start());
		TestEqual(TEXT("... not even tried"), Service->GetCounters().ContextCreations.load(), 0);
	}
	TestTrue(TEXT("once it is done, the same service starts: the refusal is not latched"), Service->Start());
	Service->Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceLogLevelTest, "VaRest.WebSocket.Lws.Service.LogLevel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceLogLevelTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
#if IS_MONOLITHIC || UE_MERGED_MODULES
	AddInfo(TEXT("a shared LWS copy: the level is the engine's when WebSockets is loaded (D30); checked in W1b's monolithic run"));
#else
	ApplyLogLevel();
	// D30 (spec 1.2): VaRest's own copy logs errors and warnings only [coexist.shared_lws_state] (lwsl_visible, libwebsockets.h:434-443)
	TestTrue(TEXT("errors are visible"), lwsl_visible(LLL_ERR) != 0);
	TestTrue(TEXT("warnings are visible"), lwsl_visible(LLL_WARN) != 0);
	TestEqual(TEXT("notices are not (LWS's default has them, libwebsockets.h:413-414)"), lwsl_visible(LLL_NOTICE), 0);
	TestEqual(TEXT("client lines are not"), lwsl_visible(LLL_CLIENT), 0);
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceStartupPolicyTest, "VaRest.WebSocket.Lws.Service.StartupPolicy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceStartupPolicyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	// Plan decision 8 (VaRest-rti)
	const auto Nothing = [](const TCHAR*) { return false; };
	const auto WebSockets = [](const TCHAR* Name) { return FCString::Strcmp(Name, TEXT("WebSockets")) == 0; };
	const auto Replay = [](const TCHAR* Name) { return FCString::Strcmp(Name, TEXT("HttpNetworkReplayStreaming")) == 0; };
	TestFalse(TEXT("a modular build: lazily, VaRest's copy is its own"), ShouldCreateAtStartup(false, WebSockets));
	TestFalse(TEXT("a shared copy with no other LWS module: lazily"), ShouldCreateAtStartup(true, Nothing));
	TestTrue(TEXT("a shared copy with WebSockets: at startup"), ShouldCreateAtStartup(true, WebSockets));
	TestTrue(TEXT("... or with any other LWS module"), ShouldCreateAtStartup(true, Replay));
	return true;
}

#endif
