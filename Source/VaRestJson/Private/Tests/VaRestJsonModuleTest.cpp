// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "VaRestJson.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonModuleLoadsTest, "VaRest.Json.Module.Loads",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonModuleLoadsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("VaRestJson module is loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("VaRestJson")));
	TestFalse(TEXT("A default handle is invalid"), FVaRestJson().IsValid());
	return true;
}

#endif
