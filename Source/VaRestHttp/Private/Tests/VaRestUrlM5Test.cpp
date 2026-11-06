// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestHttpTesting.h"
#include "VaRestUrl.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestM5PathArrayTest, "VaRest.Http.Url.M5PathArrays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestM5PathArrayTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	const TArray<FString> Values{TEXT("a,b"), TEXT("c/d"), TEXT("%"), TEXT("?x=y"), TEXT("#")};
	TArray<FPathParam> Params;
	Params.Emplace(TEXT("ids"), Values);
	auto Expanded = ExpandPathParameters(TEXT("/items/{ids}"), Params);
	if (!TestTrue(TEXT("raw path array expands"), Expanded.HasValue()))
		return false;
	TestEqualSensitive(TEXT("separators are literal but data delimiters are encoded once"), Expanded.GetValue(),
		FString(TEXT("/items/a%2Cb,c%2Fd,%25,%3Fx%3Dy,%23")));
	Params.Emplace(TEXT("ids"), TEXT("last/value"));
	Expanded = ExpandPathParameters(TEXT("/items/{ids}"), Params);
	TestTrue(TEXT("last scalar overrides the earlier array in actual argument order"), Expanded.HasValue() && Expanded.GetValue() == TEXT("/items/last%2Fvalue"));
	Params.Emplace(TEXT("ids"), TArray<FString>{TEXT("one"), TEXT("two")});
	Expanded = ExpandPathParameters(TEXT("/items/{ids}/{ids}"), Params);
	TestTrue(TEXT("last array overrides scalar and fills repeated occurrences"), Expanded.HasValue() && Expanded.GetValue() == TEXT("/items/one,two/one,two"));
	for (TArray<FString> Bad : {TArray<FString>{}, TArray<FString>{TEXT("")}, TArray<FString>{TEXT(".")}, TArray<FString>{TEXT("..")}})
	{
		TArray<FPathParam> BadParams;
		BadParams.Emplace(TEXT("ids"), MoveTemp(Bad));
		TestTrue(TEXT("empty or complete dot-segment array is refused"), ExpandPathParameters(TEXT("/{ids}"), BadParams).HasError());
	}
	TArray<FPathParam> Composite;
	Composite.Emplace(TEXT("ids"), TArray<FString>{TEXT("."), TEXT(".."), TEXT("")});
	Expanded = ExpandPathParameters(TEXT("/{ids}"), Composite);
	TestTrue(TEXT("dots within a comma segment cannot navigate hierarchy"), Expanded.HasValue() && Expanded.GetValue() == TEXT("/.,..,"));
	TestTrue(TEXT("unknown array parameters retain unused-name refusal"), ExpandPathParameters(TEXT("/items"), Composite).HasError());
	TestTrue(TEXT("exact name matching never treats IDS as ids"), ExpandPathParameters(TEXT("/{IDS}"), Composite).HasError());

	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/items/{ids}")).ParamValues(TEXT("ids"), Values), Outcome).Send();
	if (!TestTrue(TEXT("actual fluent builder reaches native transport"), Rig.Wait(Outcome)) || !TestEqual(TEXT("one native attempt"), Rig.Fake->NumCalls(), 1))
		return false;
	TestEqualSensitive(TEXT("wire URL preserves only the validated literal commas"), Rig.Fake->GetCall(0).Request.Url,
		FString(TEXT("https://api.test/items/a%2Cb,c%2Fd,%25,%3Fx%3Dy,%23")));
	return true;
}

#endif
