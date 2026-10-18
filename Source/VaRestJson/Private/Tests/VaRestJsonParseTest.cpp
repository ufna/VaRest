// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJson.h"
#include "Async/ParallelFor.h"

#include <atomic>

namespace VaRestJsonParseTest
{
	FVaRestJson P(FAutomationTestBase& Test, const TCHAR* Text)
	{
		TValueOrError<FVaRestJson, FVaRestJsonError> R = FVaRestJson::Parse(FStringView(Text));
		if (!R.HasValue())
		{
			Test.AddError(FString::Printf(TEXT("parse failed for %s: %s"), Text, *R.GetError().ToString()));
			return FVaRestJson();
		}
		return R.StealValue();
	}

	FVaRestJsonError E(FAutomationTestBase& Test, FUtf8StringView Text, const FVaRestJsonParseOptions& Options = FVaRestJsonParseOptions())
	{
		TValueOrError<FVaRestJson, FVaRestJsonError> R = FVaRestJson::Parse(Text, Options);
		if (R.HasValue())
		{
			Test.AddError(TEXT("expected a parse error"));
			return FVaRestJsonError();
		}
		return R.GetError();
	}
}
using namespace VaRestJsonParseTest;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonParseScalarsTest, "VaRest.Json.Parse.Scalars",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonParseScalarsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("null"), P(*this, TEXT("null")).IsNull());
	TestTrue(TEXT("true"), P(*this, TEXT("true")).AsBool());
	TestEqual(TEXT("int32 overflow -> int64"), P(*this, TEXT("2147483648")).AsInt64(), 2147483648ll);
	TestEqual(TEXT("int64 max"), P(*this, TEXT("9223372036854775807")).AsInt64(), MAX_int64);
	TestEqual(TEXT("int64 min"), P(*this, TEXT("-9223372036854775808")).AsInt64(), MIN_int64);
	TestEqual(TEXT("2^63 -> uint64"), P(*this, TEXT("9223372036854775808")).AsUInt64(), 9223372036854775808ull);
	TestEqual(TEXT("uint64 max"), P(*this, TEXT("18446744073709551615")).AsUInt64(), MAX_uint64);
	TestFalse(TEXT("2^64 is no longer an integer"), P(*this, TEXT("18446744073709551616")).TryGet<uint64>().IsSet());
	TestEqual(TEXT("huge integer literal -> double"), P(*this, TEXT("100000000000000000000000000000")).AsDouble(), 1e29);
	TestEqual(TEXT("1e308"), P(*this, TEXT("1e308")).AsDouble(), 1e308);
	TestEqual(TEXT("denormal"), P(*this, TEXT("5e-324")).AsDouble(), 5e-324);
	TestEqual(TEXT("full precision"), P(*this, TEXT("0.1")).AsDouble(), 0.1);
	TestEqual(TEXT("-0 is zero"), P(*this, TEXT("-0")).AsDouble(), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonParseStringsTest, "VaRest.Json.Parse.Strings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonParseStringsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("escapes"), P(*this, TEXT("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"")).AsString(), FString(TEXT("\"\\/\b\f\n\r\t")));
	TestEqual(TEXT("\\u00e9"), P(*this, TEXT("\"\\u00e9\"")).AsString(), FString(TEXT("é")));
	TestEqual(TEXT("surrogate pair -> 4-byte UTF-8"), P(*this, TEXT("\"\\ud83d\\ude00\"")).AsUtf8View().Len(), 4);
	TestEqual(TEXT("raw UTF-8"), P(*this, TEXT("\"привет\"")).AsString(), FString(TEXT("привет")));

	// Embedded NUL in a key and in a value
	const FVaRestJson J = P(*this, TEXT("{\"a\\u0000b\":\"x\\u0000y\"}"));
	const UTF8CHAR Key[] = { 'a', 0, 'b' };
	TestEqual(TEXT("key with NUL is found"), J[FUtf8StringView(Key, 3)].AsUtf8View().Len(), 3);
	TestEqual(TEXT("NUL round-trips"), J.ToString(), FString(TEXT("{\"a\\u0000b\":\"x\\u0000y\"}")));

	const uint8 BadUtf8[] = { '"', 0xC3, 0x28, '"' };
	TestTrue(TEXT("invalid UTF-8 is rejected"), !FVaRestJson::Parse(TConstArrayView<uint8>(BadUtf8, 4)).HasValue());
	TestFalse(TEXT("unpaired surrogate is rejected"), FVaRestJson::Parse(FStringView(TEXT("\"\\ud800\""))).HasValue());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonParseStructureTest, "VaRest.Json.Parse.Structure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonParseStructureTest::RunTest(const FString& Parameters)
{
	const FVaRestJson J = P(*this, TEXT(" { \"a\" : [ 1 , { \"b\" : null } , [ ] ] , \"c\" : {} } "));
	TestEqual(TEXT("nested"), J[TEXT("a")].Num(), 3);
	TestTrue(TEXT("deep null"), J[TEXT("a")][1][TEXT("b")].IsNull());
	TestEqual(TEXT("round trip"), J.ToString(), FString(TEXT("{\"a\":[1,{\"b\":null},[]],\"c\":{}}")));

	const FVaRestJson D = P(*this, TEXT("{\"a\":1,\"a\":2}"));
	TestEqual(TEXT("duplicates: last wins"), D[TEXT("a")].AsInt(), 2);
	TestEqual(TEXT("duplicates preserved"), D.Num(), 2);

	FString Big = TEXT("{");
	for (int32 i = 0; i < 1000; ++i)
	{
		Big += FString::Printf(TEXT("%s\"k%d\":%d"), i ? TEXT(",") : TEXT(""), i, i);
	}
	Big += TEXT("}");
	const FVaRestJson B = P(*this, *Big);
	TestEqual(TEXT("big object lookup"), B[TEXT("k777")].AsInt(), 777);

	// UTF-8 BOM is skipped
	const uint8 WithBom[] = { 0xEF, 0xBB, 0xBF, '{', '"', 'a', '"', ':', '1', '}' };
	TValueOrError<FVaRestJson, FVaRestJsonError> R = FVaRestJson::Parse(TConstArrayView<uint8>(WithBom, UE_ARRAY_COUNT(WithBom)));
	TestTrue(TEXT("BOM accepted"), R.HasValue() && R.GetValue()[TEXT("a")].AsInt() == 1);

	// ParseOwned steals the buffer
	TArray<uint8> Owned;
	Owned.Append(reinterpret_cast<const uint8*>("[\"s\"]"), 5);
	TValueOrError<FVaRestJson, FVaRestJsonError> O = FVaRestJson::ParseOwned(MoveTemp(Owned));
	TestTrue(TEXT("owned parse"), O.HasValue() && O.GetValue()[0].AsString() == TEXT("s"));
	TestEqual(TEXT("buffer was moved"), Owned.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonParseErrorsTest, "VaRest.Json.Parse.Errors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonParseErrorsTest::RunTest(const FString& Parameters)
{
	const FVaRestJsonError Trailing = E(*this, UTF8TEXT("{\"a\":1,}"));
	TestEqual(TEXT("trailing comma offset"), Trailing.Offset, (int64)7);
	TestEqual(TEXT("trailing comma line"), Trailing.Line, 1);

	const FVaRestJsonError Multi = E(*this, UTF8TEXT("{\n  \"a\": 1,\n  \"b\": x\n}"));
	TestEqual(TEXT("line of the error"), Multi.Line, 3);
	TestEqual(TEXT("column of the error"), Multi.Column, 8);

	TestTrue(TEXT("empty input"), !E(*this, UTF8TEXT("")).Message.IsEmpty());
	TestTrue(TEXT("trailing garbage"), !E(*this, UTF8TEXT("{} x")).Message.IsEmpty());

	FVaRestJsonParseOptions Shallow;
	Shallow.MaxDepth = 4;
	TestTrue(TEXT("depth 4 ok"), FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("[[[[1]]]]")), Shallow).HasValue());
	const FVaRestJsonError Deep = E(*this, UTF8TEXT("[[[[[1]]]]]"), Shallow);
	TestTrue(TEXT("depth 5 fails with a depth message"), Deep.Message.Contains(TEXT("deeper than 4")));

	FString Hostile;
	for (int32 i = 0; i < 100000; ++i)
	{
		Hostile += TEXT("[");
	}
	TestFalse(TEXT("100k-deep input fails cleanly (no stack overflow)"), FVaRestJson::Parse(FStringView(Hostile)).HasValue());

	FVaRestJsonParseOptions Lenient;
	Lenient.bAllowCommentsAndTrailingCommas = true;
	TestTrue(TEXT("comments + trailing comma when allowed"), FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("{\"a\":1, // c\n}")), Lenient).HasValue());

	// A stolen buffer cannot report lines, but still reports the offset
	TArray<uint8> Bad;
	Bad.Append(reinterpret_cast<const uint8*>("[1,,2]"), 6);
	TValueOrError<FVaRestJson, FVaRestJsonError> R = FVaRestJson::ParseOwned(MoveTemp(Bad));
	TestTrue(TEXT("owned error has offset"), !R.HasValue() && R.GetError().Offset == 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonParseConcurrentReadTest, "VaRest.Json.Parse.ConcurrentReadersOnParsed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonParseConcurrentReadTest::RunTest(const FString& Parameters)
{
	FString Text = TEXT("{");
	for (int32 i = 0; i < 2000; ++i)
	{
		Text += FString::Printf(TEXT("%s\"key%d\":%d"), i ? TEXT(",") : TEXT(""), i, i);
	}
	Text += TEXT("}");
	const FVaRestJson J = P(*this, *Text);
	std::atomic<int32> Wrong{ 0 };
	ParallelFor(64, [&J, &Wrong](int32 Worker)
	{
		for (int32 i = 0; i < 2000; ++i)
		{
			const int32 k = (i * 31 + Worker) % 2000;
			if (J[*FString::Printf(TEXT("key%d"), k)].AsInt(-1) != k)
			{
				++Wrong;
			}
		}
	});
	TestEqual(TEXT("concurrent lookups on a parsed object"), Wrong.load(), 0);
	return true;
}

#endif
