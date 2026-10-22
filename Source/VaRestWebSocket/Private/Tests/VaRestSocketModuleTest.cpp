// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "VaRestLwsInclude.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketModuleLwsTest, "VaRest.WebSocket.Module.Lws",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketModuleLwsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("the module is loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("VaRestWebSocket")));
	// T1: libwebsockets 3.0.0 on every target (lws_config.h:34; passport env.lws_version)
	const FString Version = UTF8_TO_TCHAR(lws_get_library_version());
	TestTrue(FString::Printf(TEXT("libwebsockets 3.0.x is linked (got \"%s\")"), *Version), Version.StartsWith(TEXT("3.0."), ESearchCase::CaseSensitive));
	// The engine's OpenSSL (ThirdParty/OpenSSL/OpenSSL.Build.cs:12; passport env.openssl_version)
	const FString OpenSsl = UTF8_TO_TCHAR(OpenSSL_version(OPENSSL_VERSION));
	TestTrue(FString::Printf(TEXT("OpenSSL 1.1.1 is linked (got \"%s\")"), *OpenSsl), OpenSsl.StartsWith(TEXT("OpenSSL 1.1.1"), ESearchCase::CaseSensitive));
	return true;
}

#endif
