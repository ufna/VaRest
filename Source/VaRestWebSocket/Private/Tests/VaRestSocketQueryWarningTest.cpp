// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeExit.h"
#include "VaRestSocketQueryWarning.h"
#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketQueryWarningPolicyTest, "VaRest.WebSocket.QueryWarning.Policy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestSocketQueryWarningPolicyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	const auto High = EObservedEngineLog::Verbose, Low = EObservedEngineLog::BelowVerbose, Unknown = EObservedEngineLog::Unknown;
	FQueryWarningLatch Policy;
	for (const TCHAR* Path : {TEXT("/socket"), TEXT("/socket?"), TEXT("/socket#fragment?token=x"), TEXT("/socket?#token=x"), TEXT("/socket%3Ftoken=x")})
		TestFalse(TEXT("only a nonempty actual query qualifies"), Policy.Take(true, true, High, Path));
	TestFalse(TEXT("separate LWS copy"), Policy.Take(false, true, High, TEXT("/?flag")));
	TestFalse(TEXT("engine module absent"), Policy.Take(true, false, High, TEXT("/?flag")));
	TestFalse(TEXT("known low verbosity"), Policy.Take(true, true, Low, TEXT("/?flag")));
	TestFalse(TEXT("compiled-out logging policy never consumes admission"), Policy.Take(true, true, High, TEXT("/?flag"), false));
	TestTrue(TEXT("a later observed risk still warns"), Policy.Take(true, true, High, TEXT("/?flag#fragment")));
	TestFalse(TEXT("one admission across different queries"), Policy.Take(true, true, High, TEXT("/other?another=y")));
	FQueryWarningLatch Conditional;
	TestTrue(TEXT("unknown level permits a conditional warning"), Conditional.Take(true, true, Unknown, TEXT("/?harmless")));
	TestFalse(TEXT("conditional warning consumes the same latch"), Conditional.Take(true, true, High, TEXT("/?token=x")));
	for (const TCHAR* Level : {TEXT("NoLogging"), TEXT("Fatal"), TEXT("Error"), TEXT("Warning"), TEXT("Display"), TEXT("Log")})
		TestTrue(TEXT("every exact low type is recognized"), ParseEngineLogListing(FString(TEXT("LogWebSockets ")) + Level) == Low);
	TestTrue(TEXT("exact verbose category"), ParseEngineLogListing(TEXT("LogWebSockets Verbose")) == High);
	TestTrue(TEXT("Core debug marker"), ParseEngineLogListing(TEXT("LogWebSockets VeryVerbose - DebugBreak")) == High);
	TestTrue(TEXT("consistent duplicates"), ParseEngineLogListing(TEXT("LogWebSockets Verbose\nLogWebSockets VeryVerbose")) == High);
	TestTrue(TEXT("substring matches do not mask the actual category"), ParseEngineLogListing(TEXT("LogWebSocketsExtra Warning\nLogWebSockets Verbose")) == High);
	for (const TCHAR* Text : {TEXT(""), TEXT("LogWebSocketsExtra Verbose"), TEXT("logwebsockets Verbose"), TEXT("LogWebSockets verbose"),
		TEXT("LogWebSockets"), TEXT("LogWebSockets Verbose extra"), TEXT("LogWebSockets Verbose - debugbreak"), TEXT("LogWebSockets Verbose\nLogWebSockets Warning")})
		TestTrue(TEXT("missing, malformed or conflicting observations stay unknown"), ParseEngineLogListing(Text) == Unknown);
	TestTrue(TEXT("oversized observations are refused"), ParseEngineLogListing(FString::ChrN(MaxEngineLogListingChars + 1, TEXT('x'))) == Unknown);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketQueryWarningObserverTest, "VaRest.WebSocket.QueryWarning.Observer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestSocketQueryWarningObserverTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	const auto Changes = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	const auto Handle = FCoreDelegates::OnLogVerbosityChanged.AddLambda(
		[Changes](const FLogCategoryName&, ELogVerbosity::Type, ELogVerbosity::Type) { Changes->fetch_add(1); });
	ON_SCOPE_EXIT { FCoreDelegates::OnLogVerbosityChanged.Remove(Handle); };
	const auto Before = Lws::ObserveEngineLogLevel(), After = Lws::ObserveEngineLogLevel();
	TestTrue(TEXT("read-only observation preserves its result"), Before == After);
	TestEqual(TEXT("observation broadcasts no category verbosity changes"), Changes->load(), 0);
#if !UE_ALLOW_EXEC_COMMANDS || NO_LOGGING
	TestTrue(TEXT("compiled-out observer stays explicitly unknown"), Before == EObservedEngineLog::Unknown);
#endif
	return true;
}

#endif
