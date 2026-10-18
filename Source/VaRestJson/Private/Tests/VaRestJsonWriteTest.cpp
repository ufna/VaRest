// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJson.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonWriteCondensedTest, "VaRest.Json.Write.Condensed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonWriteCondensedTest::RunTest(const FString& Parameters)
{
	const FVaRestJson J = FVaRestJson::Object()
		.Set(TEXT("a"), 1)
		.Set(TEXT("b"), FVaRestJson::Array(true, nullptr, TEXT("x")))
		.Set(TEXT("c"), 1.5)
		.Set(TEXT("d"), FVaRestJson::Object());
	TestEqual(TEXT("condensed"), J.ToString(), FString(TEXT("{\"a\":1,\"b\":[true,null,\"x\"],\"c\":1.5,\"d\":{}}")));
	TestEqual(TEXT("invalid handle -> empty"), FVaRestJson().ToString(), FString());
	TestEqual(TEXT("null"), FVaRestJson::Null().ToString(), FString(TEXT("null")));
	TestEqual(TEXT("scalar root"), FVaRestJson::From(TEXT("hi")).ToString(), FString(TEXT("\"hi\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonWritePrettyTest, "VaRest.Json.Write.Pretty",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonWritePrettyTest::RunTest(const FString& Parameters)
{
	const FVaRestJson J = FVaRestJson::Object().Set(TEXT("a"), 1).Set(TEXT("b"), FVaRestJson::Array(1, 2)).Set(TEXT("e"), FVaRestJson::Array());
	const FString Expected = TEXT("{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2\n  ],\n  \"e\": []\n}");
	TestEqual(TEXT("pretty, two spaces"), J.ToString(EVaRestJsonFormat::Pretty), Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonWriteValuesTest, "VaRest.Json.Write.Values",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonWriteValuesTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("0.1"), FVaRestJson::From(0.1).ToString(), FString(TEXT("0.1")));
	TestEqual(TEXT("1.0 keeps its fraction"), FVaRestJson::From(1.0).ToString(), FString(TEXT("1.0")));
	TestEqual(TEXT("1e300"), FVaRestJson::From(1e300).ToString(), FString(TEXT("1e300")));
	TestEqual(TEXT("int64 min"), FVaRestJson::From(MIN_int64).ToString(), FString(TEXT("-9223372036854775808")));
	TestEqual(TEXT("uint64 max"), FVaRestJson::From(MAX_uint64).ToString(), FString(TEXT("18446744073709551615")));

	const FVaRestJson S = FVaRestJson::From(TEXT("q\"b\\n\n t\t c\x01 Ж"));
	TestEqual(TEXT("escapes; non-ASCII stays raw UTF-8"), S.ToString(), FString(TEXT("\"q\\\"b\\\\n\\n t\\t c\\u0001 Ж\"")));

	// Embedded NUL survives (lengths, not C strings)
	const UTF8CHAR WithNul[] = { 'a', 0, 'b' };
	const FVaRestJson N = FVaRestJson::From(FUtf8StringView(WithNul, 3));
	TestEqual(TEXT("embedded NUL"), N.ToString(), FString(TEXT("\"a\\u0000b\"")));

	// Non-finite numbers cannot be JSON: written as null, with one warning per write
	AddExpectedMessage(TEXT("non-finite number"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const FVaRestJson Bad = FVaRestJson::Array(std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN());
	TestEqual(TEXT("NaN/Inf -> null"), Bad.ToString(), FString(TEXT("[null,null]")));

	TArray<uint8> Bytes = { 'x' };
	FVaRestJson::From(2).ToUtf8(Bytes);
	TestEqual(TEXT("ToUtf8 appends"), Bytes.Num(), 2);
	return true;
}

#endif
