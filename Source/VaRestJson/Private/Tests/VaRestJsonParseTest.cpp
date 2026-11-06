// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/ParallelFor.h"
#include "Misc/AutomationTest.h"
#include "VaRestJson.h"
#include "VaRestJsonDocument.h"

#include <atomic>
#include <cmath>

// Usings live inside each test function: a file-scope using would leak into the files after this one
// in a unity blob.

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonParseScalarsTest, "VaRest.Json.Parse.Scalars",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonParseScalarsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonParseTest;

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
	// "-0" is the integer 0, so it writes back as "0" (JSON.stringify(-0) is "0"); "-0.0" keeps its sign
	const FVaRestJson NegZero = P(*this, TEXT("-0"));
	TestTrue(TEXT("-0 is an integer"), NegZero.GetNodeInternal() && NegZero.GetNodeInternal()->NumKind == VaRest::Json::ENumKind::Int);
	TestEqual(TEXT("-0 reads as int64 0"), NegZero.TryGet<int64>().Get(-1), (int64)0);
	TestFalse(TEXT("-0 is not a negative zero"), std::signbit(NegZero.AsDouble()));
	TestEqualSensitive(TEXT("-0 writes as 0"), NegZero.ToString(), FString(TEXT("0")));
	const FVaRestJson NegZeroDouble = P(*this, TEXT("-0.0"));
	TestTrue(TEXT("-0.0 is a negative-zero double"), std::signbit(NegZeroDouble.AsDouble()) && NegZeroDouble.AsDouble() == 0.0);
	TestEqualSensitive(TEXT("-0.0 writes as -0.0"), NegZeroDouble.ToString(), FString(TEXT("-0.0")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonParseStringsTest, "VaRest.Json.Parse.Strings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonParseStringsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonParseTest;

	TestEqualSensitive(TEXT("escapes"), P(*this, TEXT("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"")).AsString(), FString(TEXT("\"\\/\b\f\n\r\t")));
	TestEqualSensitive(TEXT("\\u00e9"), P(*this, TEXT("\"\\u00e9\"")).AsString(), FString(TEXT("é")));
	TestEqual(TEXT("surrogate pair -> 4-byte UTF-8"), P(*this, TEXT("\"\\ud83d\\ude00\"")).AsUtf8View().Len(), 4);
	TestEqualSensitive(TEXT("raw UTF-8"), P(*this, TEXT("\"привет\"")).AsString(), FString(TEXT("привет")));

	// Embedded NUL in a key and in a value
	const FVaRestJson J = P(*this, TEXT("{\"a\\u0000b\":\"x\\u0000y\"}"));
	const UTF8CHAR Key[] = { 'a', 0, 'b' };
	TestEqual(TEXT("key with NUL is found"), J[FUtf8StringView(Key, 3)].AsUtf8View().Len(), 3);
	TestEqualSensitive(TEXT("NUL round-trips"), J.ToString(), FString(TEXT("{\"a\\u0000b\":\"x\\u0000y\"}")));

	const uint8 BadUtf8[] = { '"', 0xC3, 0x28, '"' };
	TestTrue(TEXT("invalid UTF-8 is rejected"), !FVaRestJson::Parse(TConstArrayView<uint8>(BadUtf8, 4)).HasValue());
	TestFalse(TEXT("unpaired surrogate is rejected"), FVaRestJson::Parse(FStringView(TEXT("\"\\ud800\""))).HasValue());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonParseStructureTest, "VaRest.Json.Parse.Structure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonParseStructureTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonParseTest;

	const FVaRestJson J = P(*this, TEXT(" { \"a\" : [ 1 , { \"b\" : null } , [ ] ] , \"c\" : {} } "));
	TestEqual(TEXT("nested"), J[TEXT("a")].Num(), 3);
	TestTrue(TEXT("deep null"), J[TEXT("a")][1][TEXT("b")].IsNull());
	TestEqualSensitive(TEXT("round trip"), J.ToString(), FString(TEXT("{\"a\":[1,{\"b\":null},[]],\"c\":{}}")));

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
	TestTrue(TEXT("owned parse"), O.HasValue() && O.GetValue()[0].AsString().Equals(TEXT("s"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("buffer was moved"), Owned.Num(), 0);

	// With one byte of slack for the terminator, ParseOwned keeps the caller's allocation (the HTTP layer
	// relies on it; Serialize load and ImportTextItem reserve that byte too)
	TArray<uint8> Slack;
	Slack.Reserve(6);
	Slack.Append(reinterpret_cast<const uint8*>("[\"s\"]"), 5);
	const uint8* const OriginalData = Slack.GetData();
	TValueOrError<FVaRestJson, FVaRestJsonError> Z = FVaRestJson::ParseOwned(MoveTemp(Slack));
	TestTrue(TEXT("zero-copy with slack: same data pointer"), Z.HasValue() && Z.GetValue().GetDocumentInternal()->Input.GetData() == OriginalData);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonParseErrorsTest, "VaRest.Json.Parse.Errors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonParseErrorsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonParseTest;

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
	using namespace VaRestJsonParseTest;

	FString Text = TEXT("{");
	for (int32 i = 0; i < 2000; ++i)
	{
		Text += FString::Printf(TEXT("%s\"key%d\":%d"), i ? TEXT(",") : TEXT(""), i, i);
	}
	Text += TEXT("}");
	const FVaRestJson J = P(*this, *Text);
	// The index exists before any read: readers never build it lazily (no writes on read paths)
	const VaRest::Json::FIndex* const Index = J.IsObject() ? J.GetNodeInternal()->Obj.GetIndex() : nullptr;
	TestTrue(TEXT("index built at parse time"), Index != nullptr);
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
	TestTrue(TEXT("readers left the index alone"), J.GetNodeInternal()->Obj.GetIndex() == Index);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonParseHardeningTest, "VaRest.Json.Parse.Hardening",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonParseHardeningTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonParseTest;

	// A raw NUL must not end the document silently (RapidJSON stops at NUL, reader.h:2199)
	const uint8 NulThenGarbage[] = { '{', '}', 0, 'x' };
	TValueOrError<FVaRestJson, FVaRestJsonError> A = FVaRestJson::Parse(TConstArrayView<uint8>(NulThenGarbage, 4));
	TestTrue(TEXT("{}\\0x fails"), !A.HasValue());
	if (!A.HasValue())
	{
		TestEqualSensitive(TEXT("NUL message"), A.GetError().Message, FString(TEXT("Unexpected NUL byte in input")));
		TestEqual(TEXT("NUL offset"), A.GetError().Offset, (int64)2);
	}
	const uint8 NulThenBracket[] = { '[', '1', ']', 0, ']' };
	TestFalse(TEXT("[1]\\0] fails"), FVaRestJson::Parse(TConstArrayView<uint8>(NulThenBracket, 5)).HasValue());
	const uint8 TrailingNul[] = { '[', '1', ']', 0 };
	TestFalse(TEXT("a trailing NUL is also a NUL"), FVaRestJson::Parse(TConstArrayView<uint8>(TrailingNul, 4)).HasValue());

	// MaxInputBytes
	FVaRestJsonParseOptions Small;
	Small.MaxInputBytes = 4;
	TestTrue(TEXT("within the limit"), FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("[12]")), Small).HasValue());
	const FVaRestJsonError Big = E(*this, UTF8TEXT("[1,2,3]"), Small);
	TestEqualSensitive(TEXT("size message"), Big.Message, FString(TEXT("Input of 7 bytes exceeds MaxInputBytes (4)")));
	TestEqual(TEXT("size offset"), Big.Offset, (int64)-1);
	TestFalse(TEXT("FStringView overload honours the limit"), FVaRestJson::Parse(FStringView(TEXT("[1,2,3]")), Small).HasValue());

	// Effective depth limit is reported
	FVaRestJsonParseOptions Zero;
	Zero.MaxDepth = 0;
	TestTrue(TEXT("clamped limit in message"), E(*this, UTF8TEXT("[[1]]"), Zero).Message.Contains(TEXT("deeper than 1")));

	// FStringView overload keeps line/column
	TValueOrError<FVaRestJson, FVaRestJsonError> L = FVaRestJson::Parse(FStringView(TEXT("{\n  \"a\": 1,\n  \"b\": x\n}")));
	TestTrue(TEXT("string overload line/column"), !L.HasValue() && L.GetError().Line == 3 && L.GetError().Column == 8);

	// Duplicate keys on the indexed (> 8 members) path
	FString Dup = TEXT("{");
	for (int32 i = 0; i < 12; ++i)
	{
		Dup += FString::Printf(TEXT("\"k%d\":%d,"), i, i);
	}
	Dup += TEXT("\"k3\":99}");
	const FVaRestJson D = P(*this, *Dup);
	TestEqual(TEXT("indexed duplicates: last wins"), D[TEXT("k3")].AsInt(), 99);
	TestEqual(TEXT("indexed duplicates: both counted"), D.Num(), 13);
	return true;
}

#endif
