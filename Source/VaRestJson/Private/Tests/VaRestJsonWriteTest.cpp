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
	TestEqualSensitive(TEXT("condensed"), J.ToString(), FString(TEXT("{\"a\":1,\"b\":[true,null,\"x\"],\"c\":1.5,\"d\":{}}")));
	TestEqualSensitive(TEXT("invalid handle -> empty"), FVaRestJson().ToString(), FString());
	TestEqualSensitive(TEXT("null"), FVaRestJson::Null().ToString(), FString(TEXT("null")));
	TestEqualSensitive(TEXT("scalar root"), FVaRestJson::From(TEXT("hi")).ToString(), FString(TEXT("\"hi\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonWritePrettyTest, "VaRest.Json.Write.Pretty",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonWritePrettyTest::RunTest(const FString& Parameters)
{
	const FVaRestJson J = FVaRestJson::Object().Set(TEXT("a"), 1).Set(TEXT("b"), FVaRestJson::Array(1, 2)).Set(TEXT("e"), FVaRestJson::Array());
	const FString Expected = TEXT("{\n  \"a\": 1,\n  \"b\": [\n    1,\n    2\n  ],\n  \"e\": []\n}");
	TestEqualSensitive(TEXT("pretty, two spaces"), J.ToString(EVaRestJsonFormat::Pretty), Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonWriteValuesTest, "VaRest.Json.Write.Values",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonWriteValuesTest::RunTest(const FString& Parameters)
{
	TestEqualSensitive(TEXT("0.1"), FVaRestJson::From(0.1).ToString(), FString(TEXT("0.1")));
	TestEqualSensitive(TEXT("1.0 keeps its fraction"), FVaRestJson::From(1.0).ToString(), FString(TEXT("1.0")));
	TestEqualSensitive(TEXT("1e300"), FVaRestJson::From(1e300).ToString(), FString(TEXT("1e300")));
	TestEqualSensitive(TEXT("int64 min"), FVaRestJson::From(MIN_int64).ToString(), FString(TEXT("-9223372036854775808")));
	TestEqualSensitive(TEXT("uint64 max"), FVaRestJson::From(MAX_uint64).ToString(), FString(TEXT("18446744073709551615")));

	const FVaRestJson S = FVaRestJson::From(TEXT("q\"b\\n\n t\t c\x01 Ж"));
	TestEqualSensitive(TEXT("escapes; non-ASCII stays raw UTF-8"), S.ToString(), FString(TEXT("\"q\\\"b\\\\n\\n t\\t c\\u0001 Ж\"")));

	// Embedded NUL survives (lengths, not C strings)
	const UTF8CHAR WithNul[] = { 'a', 0, 'b' };
	const FVaRestJson N = FVaRestJson::From(FUtf8StringView(WithNul, 3));
	TestEqualSensitive(TEXT("embedded NUL"), N.ToString(), FString(TEXT("\"a\\u0000b\"")));

	// Non-finite numbers cannot be JSON: written as null, with one warning per write
	AddExpectedMessage(TEXT("non-finite number"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const FVaRestJson Bad = FVaRestJson::Array(std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN());
	TestEqualSensitive(TEXT("NaN/Inf -> null"), Bad.ToString(), FString(TEXT("[null,null]")));

	TArray<uint8> Bytes = { 'x' };
	FVaRestJson::From(2).ToUtf8(Bytes);
	TestEqual(TEXT("ToUtf8 appends"), Bytes.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonWriteCapacityTest, "VaRest.Json.Write.Capacity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonWriteCapacityTest::RunTest(const FString& Parameters)
{
	// 1 MiB of ASCII string: the output buffer must grow geometrically, not reserve 6x the input up front
	FString Big;
	Big.Reserve(1 << 20);
	for (int32 i = 0; i < (1 << 20); ++i)
	{
		Big.AppendChar(TEXT('a'));
	}
	TArray<uint8> Bytes;
	FVaRestJson::From(Big).ToUtf8(Bytes);
	TestEqual(TEXT("1 MiB string length"), Bytes.Num(), (1 << 20) + 2);
	TestTrue(TEXT("no 6x over-reserve"), Bytes.Max() < 2 * Bytes.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonWriteEdgeTest, "VaRest.Json.Write.Edge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonWriteEdgeTest::RunTest(const FString& Parameters)
{
	// Embedded NUL in an object key
	const UTF8CHAR Key[] = { 'a', 0, 'b' };
	const FVaRestJson K = FVaRestJson::Object().Set(FUtf8StringView(Key, 3), 1);
	TestEqualSensitive(TEXT("embedded NUL in key"), K.ToString(), FString(TEXT("{\"a\\u0000b\":1}")));

	// Lookup miss: node-less handle prints nothing
	TestEqualSensitive(TEXT("miss handle -> empty"), FVaRestJson::Object()[TEXT("nope")].ToString(), FString());

	// ToUtf8 appends after existing bytes
	TArray<uint8> Bytes = { 'x' };
	FVaRestJson::From(2).ToUtf8(Bytes);
	TestTrue(TEXT("ToUtf8 appends x2"), Bytes.Num() == 2 && Bytes[0] == 'x' && Bytes[1] == '2');
	return true;
}

#endif
