// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/ParallelFor.h"
#include "Tasks/Task.h"
#include "Misc/AutomationTest.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonConvertTestTypes.h"

#include <atomic>

namespace VaRestJsonConvertDecodeTest
{
	FVaRestJson ParseText(const UTF8CHAR* Text)
	{
		return FVaRestJson::Parse(FUtf8StringView(Text)).StealValue();
	}

	bool HasIssueAt(const TArray<FVaRestJsonError>& Issues, const TCHAR* Path)
	{
		return Issues.ContainsByPredicate([Path](const FVaRestJsonError& Issue) { return Issue.Path.Equals(Path, ESearchCase::CaseSensitive); });
	}

	/** Levels arrays, each inside the previous one: [[[...]]] */
	FVaRestJson NestedArrays(int32 Levels)
	{
		FVaRestJson Root = FVaRestJson::Array();
		FVaRestJson Current = Root;
		for (int32 i = 1; i < Levels; ++i)
		{
			Current.Add(FVaRestJson::Array());
			Current = Current[Current.Num() - 1];
		}
		return Root;
	}

	FVaRestJsonTestAll MakeFilled()
	{
		FVaRestJsonTestAll All;
		All.bIsActive = true;
		All.Int8Value = -8;
		All.Int16Value = -16;
		All.Int32Value = -32;
		All.Int64Value = MIN_int64;
		All.ByteValue = 255;
		All.Uint16Value = 65535;
		All.Uint32Value = MAX_uint32;
		All.Uint64Value = MAX_uint64;
		All.FloatValue = 0.1f;
		All.DoubleValue = 0.1;
		All.StringValue = TEXT("Ключ \"q\"");
		All.NameValue = FName(TEXT("Nm"));
		All.TextValue = FText::FromString(TEXT("Hello"));
		All.Color = EVaRestJsonTestColor::Green;
		All.Legacy = EVaRestJsonTestLegacy::Second;
		All.Inner.Id = 1;
		All.Inner.Name = TEXT("in");
		All.Numbers = { 1, 2, 3 };
		FVaRestJsonTestInner Item;
		Item.Id = 2;
		Item.Name = TEXT("two");
		All.Items.Add(Item);
		All.Tags.Add(TEXT("a"));
		All.Tags.Add(TEXT("b"));
		All.Scores.Add(TEXT("x"), 10);
		All.ById.Add(5, TEXT("five"));
		All.ByName.Add(FName(TEXT("n")), 2);
		All.ByColor.Add(EVaRestJsonTestColor::Blue, 3);
		All.Extra = FVaRestJson::Object().Set(TEXT("raw"), FVaRestJson::Array(1, 2));
		All.When = FDateTime(2026, 10, 20, 12, 34, 56, 789);
		All.Duration = FTimespan::FromSeconds(90.5);
		All.Guid = FGuid(0x01234567, 0x89ABCDEF, 0x01234567, 0x89ABCDEF);
		All.AssetPath = FSoftObjectPath(TEXT("/Game/Maps/Entry.Entry"));
		All.SoftAsset = TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/A.A")));
		All.Bytes = { 1, 2, 255 };
		All.MaybeNumber = 5;
		All.FixedArray[0] = 4;
		All.FixedArray[1] = 5;
		All.FixedArray[2] = 6;
		return All;
	}

	void ExpectSame(FAutomationTestBase& Test, const FVaRestJsonTestAll& A, const FVaRestJsonTestAll& B)
	{
		Test.TestTrue(TEXT("bool"), A.bIsActive == B.bIsActive);
		Test.TestTrue(TEXT("signed"), A.Int8Value == B.Int8Value && A.Int16Value == B.Int16Value && A.Int32Value == B.Int32Value && A.Int64Value == B.Int64Value);
		Test.TestTrue(TEXT("unsigned"), A.ByteValue == B.ByteValue && A.Uint16Value == B.Uint16Value && A.Uint32Value == B.Uint32Value && A.Uint64Value == B.Uint64Value);
		Test.TestTrue(TEXT("float is exact"), A.FloatValue == B.FloatValue);
		Test.TestTrue(TEXT("double"), A.DoubleValue == B.DoubleValue);
		Test.TestTrue(TEXT("string"), A.StringValue.Equals(B.StringValue, ESearchCase::CaseSensitive));
		Test.TestTrue(TEXT("name"), A.NameValue.ToString().Equals(B.NameValue.ToString(), ESearchCase::CaseSensitive));
		Test.TestTrue(TEXT("text"), A.TextValue.ToString().Equals(B.TextValue.ToString(), ESearchCase::CaseSensitive));
		Test.TestTrue(TEXT("enums"), A.Color == B.Color && A.Legacy == B.Legacy);
		Test.TestTrue(TEXT("nested"), A.Inner.Id == B.Inner.Id && A.Inner.Name.Equals(B.Inner.Name, ESearchCase::CaseSensitive));
		Test.TestTrue(TEXT("arrays"), A.Numbers == B.Numbers && A.Items.Num() == B.Items.Num() && A.Items.Num() > 0 && A.Items[0].Id == B.Items[0].Id);
		Test.TestTrue(TEXT("set"), A.Tags.Num() == B.Tags.Num() && A.Tags.Contains(TEXT("a")) && A.Tags.Contains(TEXT("b")));
		Test.TestTrue(TEXT("maps"), A.Scores.FindRef(TEXT("x")) == B.Scores.FindRef(TEXT("x")) && A.ById.FindRef(5).Equals(B.ById.FindRef(5), ESearchCase::CaseSensitive)
			&& A.ByName.FindRef(FName(TEXT("n"))) == B.ByName.FindRef(FName(TEXT("n"))) && A.ByColor.FindRef(EVaRestJsonTestColor::Blue) == B.ByColor.FindRef(EVaRestJsonTestColor::Blue));
		Test.TestTrue(TEXT("FVaRestJson field"), A.Extra.Equals(B.Extra));
		Test.TestTrue(TEXT("date"), A.When == B.When);
		Test.TestTrue(TEXT("timespan"), A.Duration == B.Duration);
		Test.TestTrue(TEXT("guid"), A.Guid == B.Guid);
		Test.TestTrue(TEXT("soft path"), A.AssetPath == B.AssetPath);
		Test.TestTrue(TEXT("soft object"), A.SoftAsset.ToSoftObjectPath() == B.SoftAsset.ToSoftObjectPath());
		Test.TestTrue(TEXT("bytes"), A.Bytes == B.Bytes);
		Test.TestTrue(TEXT("optional"), A.MaybeNumber == B.MaybeNumber);
		Test.TestTrue(TEXT("C-style array"), A.FixedArray[0] == B.FixedArray[0] && A.FixedArray[1] == B.FixedArray[1] && A.FixedArray[2] == B.FixedArray[2]);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeRoundTripTest, "VaRest.Json.Convert.Decode.RoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	const FVaRestJsonTestAll Filled = MakeFilled();
	for (int32 Variant = 0; Variant < 2; ++Variant)
	{
		FVaRestJsonConvertOptions Options;
		Options.bEnumsAsNumbers = Variant == 1;
		Options.bBytesAsBase64 = Variant == 1;
		TArray<uint8> Bytes;
		TestTrue(TEXT("encode"), FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestAll::StaticStruct(), &Filled, Bytes, Options));
		FVaRestJsonTestAll Decoded;
		FVaRestJsonDecodeReport Report;
		// Decoded with default options: numeric enums and base64 bytes are accepted either way
		TestTrue(TEXT("decode"), FVaRestJsonConvert::Utf8ToStruct(Bytes, FVaRestJsonTestAll::StaticStruct(), &Decoded, FVaRestJsonConvertOptions(), &Report));
		TestEqual(TEXT("a round trip has no warnings"), Report.Warnings.Num(), 0);
		ExpectSame(*this, Decoded, Filled);
		TestFalse(TEXT("an FVaRestJson field gets its own document"), Decoded.Extra.GetDocumentInternal() == Filled.Extra.GetDocumentInternal());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeTolerantNamesTest, "VaRest.Json.Convert.Decode.TolerantNames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeTolerantNamesTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	FVaRestJsonTestNaming Naming;
	const auto Decode = [&Naming](const UTF8CHAR* Text, const FVaRestJsonConvertOptions& Options)
	{
		Naming = FVaRestJsonTestNaming();
		FVaRestJsonDecodeReport Report;
		FVaRestJsonConvert::JsonToStruct(ParseText(Text), FVaRestJsonTestNaming::StaticStruct(), &Naming, Options, &Report);
		return Report;
	};
	const FVaRestJsonConvertOptions Defaults;
	Decode(UTF8TEXT("{\"userId\":1}"), Defaults);
	TestEqual(TEXT("exact"), Naming.UserId, 1);
	Decode(UTF8TEXT("{\"USERID\":2}"), Defaults);
	TestEqual(TEXT("case-insensitive"), Naming.UserId, 2);
	Decode(UTF8TEXT("{\"user_id\":3}"), Defaults);
	TestEqual(TEXT("normalised"), Naming.UserId, 3);
	Decode(UTF8TEXT("{\"user_id\":5,\"userId\":6}"), Defaults);
	TestEqual(TEXT("exact wins"), Naming.UserId, 6);
	Decode(UTF8TEXT("{\"userId\":6,\"user_id\":5}"), Defaults);
	TestEqual(TEXT("exact wins even when the tolerant key comes later"), Naming.UserId, 6);
	Decode(UTF8TEXT("{\"userId\":1,\"userId\":9}"), Defaults);
	TestEqual(TEXT("last occurrence within a tier"), Naming.UserId, 9);
	Decode(UTF8TEXT("{\"is_active\":true,\"url_path\":\"/x\",\"http_status2\":7,\"userName\":\"n\"}"), Defaults);
	TestTrue(TEXT("every field by another policy's spelling"), Naming.bIsActive && Naming.URLPath.Equals(TEXT("/x"), ESearchCase::CaseSensitive)
		&& Naming.HTTPStatus2 == 7 && Naming.user_name.Equals(TEXT("n"), ESearchCase::CaseSensitive));
	const FVaRestJsonDecodeReport Unknown = Decode(UTF8TEXT("{\"nope\":1,\"userId\":1}"), Defaults);
	TestEqual(TEXT("unknown keys are ignored silently"), Unknown.Warnings.Num(), 0);

	FVaRestJsonConvertOptions Snake;
	Snake.Naming = EVaRestJsonNaming::SnakeCase;
	Decode(UTF8TEXT("{\"userId\":4}"), Snake);
	TestEqual(TEXT("under snake_case, camelCase is a tolerant match"), Naming.UserId, 4);
	FVaRestJsonConvertOptions Strict;
	Strict.bStrict = true;
	Decode(UTF8TEXT("{\"USERID\":2}"), Strict);
	TestEqual(TEXT("names stay tolerant in strict mode"), Naming.UserId, 2);

	// Review Focus, decode half of the field-name collision (Task 6 keeps user_id and skips UserId): the shared key
	// fills the surviving field only. A fresh map build logs the skip once.
	FVaRestJsonConvert::ResetCache();
	AddExpectedMessagePlain(TEXT("is already used by another field"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FVaRestJsonTestCollision Collision;
	FVaRestJsonDecodeReport CollisionReport;
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"userId\":5}")), FVaRestJsonTestCollision::StaticStruct(), &Collision, Defaults, &CollisionReport);
	TestTrue(TEXT("a key two fields map to fills the first field only"), Collision.user_id == 5 && Collision.UserId == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeCoercionsTest, "VaRest.Json.Convert.Decode.Coercions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeCoercionsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	const FVaRestJson Input = ParseText(UTF8TEXT("{\"int32Value\":\"42\",\"isActive\":1,\"stringValue\":42,\"doubleValue\":\"2.5\",\"int16Value\":\"x\",\"int8Value\":300}"));

	FVaRestJsonTestAll Lenient;
	FVaRestJsonDecodeReport Report;
	TestTrue(TEXT("lenient decode succeeds"), FVaRestJsonConvert::JsonToStruct(Input, FVaRestJsonTestAll::StaticStruct(), &Lenient, FVaRestJsonConvertOptions(), &Report));
	TestEqual(TEXT("\"42\" -> 42"), Lenient.Int32Value, 42);
	TestTrue(TEXT("1 -> true"), Lenient.bIsActive);
	TestEqualSensitive(TEXT("42 -> \"42\""), Lenient.StringValue, FString(TEXT("42")));
	TestTrue(TEXT("\"2.5\" -> 2.5"), Lenient.DoubleValue == 2.5);
	TestEqual(TEXT("\"x\" keeps the default"), static_cast<int32>(Lenient.Int16Value), 0);
	TestEqual(TEXT("out of range keeps the default"), static_cast<int32>(Lenient.Int8Value), 0);
	TestEqual(TEXT("four coercions and two mismatches are warnings"), Report.Warnings.Num(), 6);
	TestEqual(TEXT("no errors when lenient"), Report.Errors.Num(), 0);
	TestTrue(TEXT("warnings carry the JSON path"), HasIssueAt(Report.Warnings, TEXT("int8Value")) && HasIssueAt(Report.Warnings, TEXT("int32Value")));

	FVaRestJsonTestAll Strict;
	FVaRestJsonDecodeReport StrictReport;
	FVaRestJsonConvertOptions StrictOptions;
	StrictOptions.bStrict = true;
	TestFalse(TEXT("strict decode fails"), FVaRestJsonConvert::JsonToStruct(Input, FVaRestJsonTestAll::StaticStruct(), &Strict, StrictOptions, &StrictReport));
	TestEqual(TEXT("strict: no number from a string"), Strict.Int32Value, 0);
	TestFalse(TEXT("strict: no bool from 1"), Strict.bIsActive);
	TestEqual(TEXT("strict: the same six are errors"), StrictReport.Errors.Num(), 6);
	TestEqual(TEXT("strict: no warnings"), StrictReport.Warnings.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeNullTest, "VaRest.Json.Convert.Decode.NullAndMissing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeNullTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	FVaRestJsonTestAll All = MakeFilled();
	FVaRestJsonDecodeReport Report;
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"inner\":null,\"numbers\":null,\"scores\":null,\"maybeNumber\":null,\"extra\":null,\"when\":null,\"int32Value\":null}")),
		FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &Report);
	TestEqual(TEXT("null keeps a struct"), All.Inner.Id, 1);
	TestTrue(TEXT("null keeps a date"), All.When == FDateTime(2026, 10, 20, 12, 34, 56, 789));
	TestEqual(TEXT("null clears an array"), All.Numbers.Num(), 0);
	TestEqual(TEXT("null clears a map"), All.Scores.Num(), 0);
	TestFalse(TEXT("null unsets an optional"), All.MaybeNumber.IsSet());
	TestTrue(TEXT("null into FVaRestJson is JSON null"), All.Extra.IsNull());
	TestEqual(TEXT("null into an int keeps it"), All.Int32Value, -32);
	TestEqual(TEXT("... with one warning"), Report.Warnings.Num(), 1);
	TestTrue(TEXT("absent fields keep their values"), All.Int64Value == MIN_int64 && All.Tags.Num() == 2);

	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"items\":[{\"name\":\"only\"}],\"numbers\":[9]}")), FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &Report);
	TestTrue(TEXT("containers are replaced, not appended"), All.Numbers == TArray<int32>({ 9 }) && All.Items.Num() == 1);
	TestEqual(TEXT("a new element starts from defaults"), All.Items.Num() == 1 ? All.Items[0].Id : -1, 0);

	All.MaybeNumber = 5;
	FVaRestJsonDecodeReport Mismatch;
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"maybeNumber\":\"x\"}")), FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &Mismatch);
	TestTrue(TEXT("a mismatch keeps a set optional"), All.MaybeNumber.IsSet() && *All.MaybeNumber == 5 && Mismatch.Warnings.Num() == 1);
	All.MaybeNumber.Reset();
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"maybeNumber\":\"x\"}")), FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &Mismatch);
	TestFalse(TEXT("... and an unset one unset"), All.MaybeNumber.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodePathsTest, "VaRest.Json.Convert.Decode.WarningsHavePaths",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodePathsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	const FVaRestJson Input = ParseText(UTF8TEXT("{\"items\":[{\"id\":1},{\"id\":\"x\"}],\"byName\":{\"k\":\"y\"}}"));
	FVaRestJsonTestAll All;
	FVaRestJsonDecodeReport Report;
	FVaRestJsonConvert::JsonToStruct(Input, FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &Report);
	TestEqual(TEXT("two warnings"), Report.Warnings.Num(), 2);
	TestTrue(TEXT("array element path"), HasIssueAt(Report.Warnings, TEXT("items[1].id")));
	TestTrue(TEXT("map value path"), HasIssueAt(Report.Warnings, TEXT("byName.k")));
	TestFalse(TEXT("a map value that does not decode is skipped"), All.ByName.Contains(FName(TEXT("k"))));

	// Without a report: one summary line per decode, never one per issue
	AddExpectedMessagePlain(TEXT("decoding VaRestJsonTestAll: 2 warning(s), 0 error(s); first at 'items[1].id'"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FVaRestJsonTestAll Quiet;
	FVaRestJsonConvert::JsonToStruct(Input, FVaRestJsonTestAll::StaticStruct(), &Quiet);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeContainersTest, "VaRest.Json.Convert.Decode.Containers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeContainersTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	FVaRestJsonTestAll All;
	FVaRestJsonDecodeReport Report;
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT(
		"{\"tags\":[\"a\",\"a\",\"b\"],\"scores\":{\"a\":1,\"A\":2},\"byId\":{\"5\":\"five\",\"x\":\"bad\",\"07\":\"lead\"},"
		"\"byColor\":{\"Blue\":3,\"1\":4},\"bytes\":\"AQL/\",\"fixedArray\":[1,2,3,4]}")),
		FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &Report);
	TestEqual(TEXT("a set collapses duplicates"), All.Tags.Num(), 2);
	// Review Focus: FString map keys ignore case in UE; "a" and "A" become one pair, the last value wins, no broken map
	TestEqual(TEXT("case-variant keys collapse"), All.Scores.Num(), 1);
	TestEqual(TEXT("... the last value wins"), All.Scores.FindRef(TEXT("a")), 2);
	TestTrue(TEXT("... with the last key's spelling"), All.Scores.Num() == 1 && All.Scores.begin()->Key.Equals(TEXT("A"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("integer keys"), All.ById.Num() == 1 && All.ById.FindRef(5).Equals(TEXT("five"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("bad integer keys are warnings"), HasIssueAt(Report.Warnings, TEXT("byId.x")) && HasIssueAt(Report.Warnings, TEXT("byId.07")));
	TestTrue(TEXT("enum keys by name and by number"), All.ByColor.FindRef(EVaRestJsonTestColor::Blue) == 3 && All.ByColor.FindRef(EVaRestJsonTestColor::Green) == 4);
	TestTrue(TEXT("bytes from base64"), All.Bytes == TArray<uint8>({ 1, 2, 255 }));
	TestTrue(TEXT("a C-style array takes what fits"), All.FixedArray[0] == 1 && All.FixedArray[2] == 3);
	TestTrue(TEXT("... and warns about the rest"), HasIssueAt(Report.Warnings, TEXT("fixedArray")));

	FVaRestJsonDecodeReport Bytes;
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"bytes\":[1,256,3]}")), FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &Bytes);
	TestTrue(TEXT("bytes from numbers; an out-of-range element stays 0"), All.Bytes == TArray<uint8>({ 1, 0, 3 }) && HasIssueAt(Bytes.Warnings, TEXT("bytes[1]")));

	// A negative integer fits no unsigned key or field. Asked through CanHoldValue(int64), a uint64 key would take -1 as
	// 18446744073709551615 and merge it with that key.
	FVaRestJsonTestUnsigned Unsigned;
	FVaRestJsonDecodeReport UnsignedReport;
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"byBig\":{\"-1\":1,\"18446744073709551615\":2},\"bySmall\":{\"-1\":1,\"255\":2},\"big\":-1,\"mid\":-1}")),
		FVaRestJsonTestUnsigned::StaticStruct(), &Unsigned, FVaRestJsonConvertOptions(), &UnsignedReport);
	TestTrue(TEXT("-1 is no uint64 key"), Unsigned.ByBig.Num() == 1 && Unsigned.ByBig.FindRef(MAX_uint64) == 2 && HasIssueAt(UnsignedReport.Warnings, TEXT("byBig.-1")));
	TestTrue(TEXT("... nor a uint8 key"), Unsigned.BySmall.Num() == 1 && Unsigned.BySmall.FindRef(255) == 2 && HasIssueAt(UnsignedReport.Warnings, TEXT("bySmall.-1")));
	TestTrue(TEXT("unsigned fields keep their value"), Unsigned.Big == 7 && Unsigned.Mid == 7 && HasIssueAt(UnsignedReport.Warnings, TEXT("big"))
		&& HasIssueAt(UnsignedReport.Warnings, TEXT("mid")));
	TestEqual(TEXT("four mismatches"), UnsignedReport.Warnings.Num(), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeBytesResolverTest, "VaRest.Json.Convert.Decode.BytesResolver",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeBytesResolverTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	// A Socket.IO-style binary placeholder: the bytes come from elsewhere (WebSocket spec §19 item 10)
	const TArray<uint8> Attachment = { 9, 8, 7 };
	FString SeenPath;
	FVaRestJsonConvertOptions Options;
	Options.BytesResolver = [&Attachment, &SeenPath](const FVaRestJson& Value, const FString& Path, TArray<uint8>& OutBytes)
	{
		if (!Value[TEXT("_placeholder")].AsBool())
		{
			return false; // not ours: the built-in forms apply
		}
		SeenPath = Path;
		OutBytes = Attachment;
		return true;
	};
	FVaRestJsonTestAll All;
	FVaRestJsonDecodeReport Report;
	TestTrue(TEXT("decodes"), FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"bytes\":{\"_placeholder\":true,\"num\":0}}")), FVaRestJsonTestAll::StaticStruct(), &All, Options, &Report));
	TestTrue(TEXT("the resolver supplied the bytes"), All.Bytes == Attachment && Report.Warnings.Num() == 0);
	TestEqualSensitive(TEXT("... for the value's path"), SeenPath, FString(TEXT("bytes")));
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"bytes\":[1,2]}")), FVaRestJsonTestAll::StaticStruct(), &All, Options, &Report);
	TestTrue(TEXT("declined: the built-in forms still apply"), All.Bytes == TArray<uint8>({ 1, 2 }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeEnumsTest, "VaRest.Json.Convert.Decode.Enums",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeEnumsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	FVaRestJsonTestAll All;
	FVaRestJsonDecodeReport Report;
	const auto Color = [&All, &Report](const UTF8CHAR* Text, bool bStrictMode)
	{
		All.Color = EVaRestJsonTestColor::Red;
		Report = FVaRestJsonDecodeReport();
		FVaRestJsonConvertOptions Options;
		Options.bStrict = bStrictMode;
		FVaRestJsonConvert::JsonToStruct(ParseText(Text), FVaRestJsonTestAll::StaticStruct(), &All, Options, &Report);
		return All.Color;
	};
	TestTrue(TEXT("by name"), Color(UTF8TEXT("{\"color\":\"Green\"}"), false) == EVaRestJsonTestColor::Green && Report.Warnings.Num() == 0);
	TestTrue(TEXT("by name ignoring case, with a warning"), Color(UTF8TEXT("{\"color\":\"green\"}"), false) == EVaRestJsonTestColor::Green && Report.Warnings.Num() == 1);
	TestTrue(TEXT("strict refuses a case-different name"), Color(UTF8TEXT("{\"color\":\"green\"}"), true) == EVaRestJsonTestColor::Red && Report.Errors.Num() == 1);
	TestTrue(TEXT("by value"), Color(UTF8TEXT("{\"color\":2}"), false) == EVaRestJsonTestColor::Blue);
	TestTrue(TEXT("an unknown value keeps the field"), Color(UTF8TEXT("{\"color\":99}"), false) == EVaRestJsonTestColor::Red && Report.Warnings.Num() == 1);
	FVaRestJsonDecodeReport LegacyReport;
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"legacy\":\"Second\"}")), FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &LegacyReport);
	TestTrue(TEXT("a byte enum by name"), All.Legacy == EVaRestJsonTestLegacy::Second);

	// An enum map key spelled in another case is a coercion too, as for an enum field
	const auto ColorKeys = [&All, &Report](bool bStrictMode)
	{
		All.ByColor.Reset();
		Report = FVaRestJsonDecodeReport();
		FVaRestJsonConvertOptions Options;
		Options.bStrict = bStrictMode;
		FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"byColor\":{\"blue\":3}}")), FVaRestJsonTestAll::StaticStruct(), &All, Options, &Report);
	};
	ColorKeys(false);
	TestTrue(TEXT("an enum key ignoring case, with a warning"), All.ByColor.FindRef(EVaRestJsonTestColor::Blue) == 3 && Report.Warnings.Num() == 1
		&& HasIssueAt(Report.Warnings, TEXT("byColor.blue")));
	ColorKeys(true);
	TestTrue(TEXT("strict refuses it"), All.ByColor.Num() == 0 && Report.Errors.Num() == 1 && HasIssueAt(Report.Errors, TEXT("byColor.blue")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeDepthTest, "VaRest.Json.Convert.Decode.Depth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeDepthTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	// UHT refuses a struct that nests itself (see FVaRestJsonTestTree), so the depth lives in the tree's FVaRestJson
	// elements: the struct's object and Children array are two levels, and an element's copy counts on from there.
	// 300 levels of {"children":[...]}, built through Set (unchecked on the way in): 600 containers
	FVaRestJson Root = FVaRestJson::Object();
	FVaRestJson Current = Root;
	for (int32 i = 0; i < 300; ++i)
	{
		Current.Set(TEXT("children"), FVaRestJson::Array(FVaRestJson::Object()));
		Current = Current[TEXT("children")][0];
	}
	FVaRestJsonTestTree Tree;
	FVaRestJsonDecodeReport Report;
	TestFalse(TEXT("too deep to decode, lenient or not"), FVaRestJsonConvert::JsonToStruct(Root, FVaRestJsonTestTree::StaticStruct(), &Tree, FVaRestJsonConvertOptions(), &Report));
	TestTrue(TEXT("one error says why"), Report.Errors.Num() == 1 && Report.Errors[0].Message.Contains(TEXT("deeper than 512")));

	// Just past the limit: the struct's two levels plus 511 nested arrays in an element are 513 containers
	const FVaRestJson PastLimit = FVaRestJson::Object().Set(TEXT("children"), FVaRestJson::Array(NestedArrays(511)));
	FVaRestJsonTestTree PastTree;
	FVaRestJsonDecodeReport PastReport;
	TestFalse(TEXT("513 containers do not decode"), FVaRestJsonConvert::JsonToStruct(PastLimit, FVaRestJsonTestTree::StaticStruct(), &PastTree, FVaRestJsonConvertOptions(), &PastReport));
	TestTrue(TEXT("... one depth error, at the element"), PastReport.Errors.Num() == 1 && PastReport.Errors[0].Message.Contains(TEXT("deeper than 512"))
		&& PastReport.Errors[0].Path.Equals(TEXT("children[0]"), ESearchCase::CaseSensitive));

	// At the limit: 512 containers, what the parser accepts (VaRest.Json.Depth.AtLimitWorks). The limit exists to bound
	// stack use on worker threads (spec 2.2), so encode and decode run there.
	FVaRestJsonTestTree AtLimit;
	AtLimit.Children.Add(NestedArrays(510));
	bool bWorkerEncoded = false;
	bool bWorkerDecoded = false;
	bool bWorkerSame = false;
	UE::Tasks::Launch(UE_SOURCE_LOCATION, [&AtLimit, &bWorkerEncoded, &bWorkerDecoded, &bWorkerSame]
	{
		TArray<uint8> Bytes;
		bWorkerEncoded = FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestTree::StaticStruct(), &AtLimit, Bytes);
		FVaRestJsonTestTree Decoded;
		FVaRestJsonDecodeReport WorkerReport;
		bWorkerDecoded = bWorkerEncoded && FVaRestJsonConvert::Utf8ToStruct(Bytes, FVaRestJsonTestTree::StaticStruct(), &Decoded, FVaRestJsonConvertOptions(), &WorkerReport);
		bWorkerSame = bWorkerDecoded && Decoded.Children.Num() == 1 && Decoded.Children[0].Equals(AtLimit.Children[0]);
	}).Wait();
	TestTrue(TEXT("512 containers encode on a worker thread"), bWorkerEncoded);
	TestTrue(TEXT("... and decode there"), bWorkerDecoded);
	TestTrue(TEXT("... to the same value"), bWorkerSame);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeConcurrentTest, "VaRest.Json.Convert.Decode.ConcurrentReaders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeConcurrentTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	// One parsed response decoded by many threads at once (spec 2.2 thread contract): reads only, shared field maps
	const FVaRestJsonTestAll Filled = MakeFilled();
	const FVaRestJson Json = FVaRestJsonConvert::StructToJson(FVaRestJsonTestAll::StaticStruct(), &Filled);
	FVaRestJsonConvert::ResetCache();
	std::atomic<int32> Wrong{ 0 };
	ParallelFor(32, [&Json, &Wrong](int32 Worker)
	{
		for (int32 i = 0; i < 20; ++i)
		{
			FVaRestJsonTestAll Decoded;
			FVaRestJsonDecodeReport Report;
			if (!FVaRestJsonConvert::JsonToStruct(Json, FVaRestJsonTestAll::StaticStruct(), &Decoded, FVaRestJsonConvertOptions(), &Report)
				|| Decoded.Int64Value != MIN_int64 || Decoded.Items.Num() != 1 || Report.Warnings.Num() != 0)
			{
				++Wrong;
			}
		}
	});
	TestEqual(TEXT("every concurrent decode is right"), Wrong.load(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodePropertyTest, "VaRest.Json.Convert.Decode.Property",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodePropertyTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	// JsonToProperty reads the shapes PropertyToJson writes (the BP wildcard nodes' pair), a C-style array included
	const UScriptStruct* Struct = FVaRestJsonTestAll::StaticStruct();
	FVaRestJsonTestAll Source;
	Source.FixedArray[0] = 7;
	Source.FixedArray[1] = 8;
	Source.FixedArray[2] = 9;
	Source.Items.AddDefaulted_GetRef().Name = TEXT("x");
	FVaRestJsonTestAll Target;
	for (const TCHAR* FieldName : { TEXT("FixedArray"), TEXT("Items") })
	{
		const FProperty* Property = Struct->FindPropertyByName(FieldName);
		const FVaRestJson Written = FVaRestJsonConvert::PropertyToJson(Property, Property->ContainerPtrToValuePtr<void>(&Source));
		TestTrue(FString::Printf(TEXT("%s decodes from what PropertyToJson wrote"), FieldName),
			FVaRestJsonConvert::JsonToProperty(Written, Property, Property->ContainerPtrToValuePtr<void>(&Target)));
	}
	TestTrue(TEXT("C-style array"), Target.FixedArray[0] == 7 && Target.FixedArray[1] == 8 && Target.FixedArray[2] == 9);
	TestTrue(TEXT("array of structs"), Target.Items.Num() == 1 && Target.Items[0].Name.Equals(TEXT("x"), ESearchCase::CaseSensitive));

	// True when the value was stored: a coercion stores it (with a warning), a mismatch does not
	const FProperty* IntProperty = Struct->FindPropertyByName(TEXT("Int32Value"));
	void* IntValue = IntProperty->ContainerPtrToValuePtr<void>(&Target);
	FVaRestJsonDecodeReport Report;
	TestTrue(TEXT("a coerced value is stored"), FVaRestJsonConvert::JsonToProperty(ParseText(UTF8TEXT("\"42\"")), IntProperty, IntValue, FVaRestJsonConvertOptions(), &Report)
		&& Target.Int32Value == 42 && Report.Warnings.Num() == 1);
	Report = FVaRestJsonDecodeReport();
	TestFalse(TEXT("a mismatch is not"), FVaRestJsonConvert::JsonToProperty(ParseText(UTF8TEXT("\"x\"")), IntProperty, IntValue, FVaRestJsonConvertOptions(), &Report));
	TestTrue(TEXT("... the value is kept, the warning is at the top-level path"), Target.Int32Value == 42 && Report.Warnings.Num() == 1
		&& Report.Warnings[0].Path.IsEmpty());

	// A property PropertyToJson refuses is refused here too, before anything is written: [] must not empty an array of
	// object references
	FVaRestJsonTestSkipped Skipped;
	Skipped.Objects.Add(nullptr);
	const FProperty* ObjectsProperty = FVaRestJsonTestSkipped::StaticStruct()->FindPropertyByName(TEXT("Objects"));
	Report = FVaRestJsonDecodeReport();
	TestFalse(TEXT("an unsupported property is refused"), FVaRestJsonConvert::JsonToProperty(ParseText(UTF8TEXT("[]")), ObjectsProperty,
		ObjectsProperty->ContainerPtrToValuePtr<void>(&Skipped), FVaRestJsonConvertOptions(), &Report));
	TestTrue(TEXT("... untouched, with one error"), Skipped.Objects.Num() == 1 && Report.Errors.Num() == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeTimespanRangeTest, "VaRest.Json.Convert.Decode.TimespanRange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeTimespanRangeTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	// FTimespan::FromSeconds casts to int64 ticks unchecked (Timespan.h:673-676), and the seconds encode writes for
	// MaxValue round to 2^63 ticks, one past int64: the extremes must still round-trip
	for (const FTimespan Extreme : { FTimespan::MaxValue(), FTimespan::MinValue() })
	{
		FVaRestJsonTestAll All;
		All.Duration = Extreme;
		TArray<uint8> Bytes;
		FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestAll::StaticStruct(), &All, Bytes);
		FVaRestJsonTestAll Decoded;
		FVaRestJsonDecodeReport Report;
		FVaRestJsonConvert::Utf8ToStruct(Bytes, FVaRestJsonTestAll::StaticStruct(), &Decoded, FVaRestJsonConvertOptions(), &Report);
		TestTrue(FString::Printf(TEXT("%lld ticks round-trip"), Extreme.GetTicks()), Decoded.Duration == Extreme && Report.Warnings.Num() == 0);
	}
	FVaRestJsonTestAll Kept;
	Kept.Duration = FTimespan::FromSeconds(5.0);
	FVaRestJsonDecodeReport Report;
	FVaRestJsonConvert::JsonToStruct(ParseText(UTF8TEXT("{\"duration\":1e300}")), FVaRestJsonTestAll::StaticStruct(), &Kept, FVaRestJsonConvertOptions(), &Report);
	TestTrue(TEXT("seconds past the range keep the field"), Kept.Duration == FTimespan::FromSeconds(5.0) && HasIssueAt(Report.Warnings, TEXT("duration")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeNameLengthTest, "VaRest.Json.Convert.Decode.NameLength",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeNameLengthTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	// 1024 characters or more abort FName construction in builds with checks (UnrealNames.cpp:3271-3281): text that long
	// from a response is a mismatch for an FName field, an FName map key, an object path and a soft pointer
	const FString Long = FString::ChrN(2000, TEXT('a'));
	const FVaRestJson Input = FVaRestJson::Object()
		.Set(TEXT("nameValue"), Long)
		.Set(TEXT("byName"), FVaRestJson::Object().Set(*Long, 1).Set(TEXT("k"), 2))
		.Set(TEXT("assetPath"), TEXT("/") + Long)
		.Set(TEXT("softAsset"), TEXT("/Game/") + Long + TEXT(".A"));
	for (int32 Variant = 0; Variant < 2; ++Variant)
	{
		const bool bStrictMode = Variant == 1;
		FVaRestJsonTestAll All = MakeFilled();
		FVaRestJsonDecodeReport Report;
		FVaRestJsonConvertOptions Options;
		Options.bStrict = bStrictMode;
		FVaRestJsonConvert::JsonToStruct(Input, FVaRestJsonTestAll::StaticStruct(), &All, Options, &Report);
		const TArray<FVaRestJsonError>& Issues = bStrictMode ? Report.Errors : Report.Warnings;
		const FString Mode = bStrictMode ? TEXT("strict: ") : TEXT("lenient: ");
		TestTrue(Mode + TEXT("an FName field keeps its value"), All.NameValue == FName(TEXT("Nm")) && HasIssueAt(Issues, TEXT("nameValue")));
		TestTrue(Mode + TEXT("an FName map key is skipped"), All.ByName.Num() == 1 && All.ByName.FindRef(FName(TEXT("k"))) == 2
			&& Issues.ContainsByPredicate([](const FVaRestJsonError& Issue) { return Issue.Path.StartsWith(TEXT("byName.aaa"), ESearchCase::CaseSensitive); }));
		TestTrue(Mode + TEXT("an object path keeps its value"), All.AssetPath == FSoftObjectPath(TEXT("/Game/Maps/Entry.Entry")) && HasIssueAt(Issues, TEXT("assetPath")));
		TestTrue(Mode + TEXT("a soft pointer keeps its value"), All.SoftAsset.ToSoftObjectPath() == FSoftObjectPath(TEXT("/Game/A.A")) && HasIssueAt(Issues, TEXT("softAsset")));
		TestEqual(Mode + TEXT("four issues"), Issues.Num(), 4);
	}
	TestFalse(TEXT("TryGet<FName> is unset too"), FVaRestJson::From(Long).TryGet<FName>().IsSet());
	TestTrue(TEXT("... while 1023 characters are a name"), FVaRestJson::From(FString::ChrN(1023, TEXT('a'))).TryGet<FName>().IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeBoundsTest, "VaRest.Json.Convert.Decode.Bounds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeBoundsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertDecodeTest;
	// Hostile input: 10 000 mismatches, a 1 MB map key, and a 1 MB key that normalises to "numbers". The report keeps 100
	// issues and counts the rest; the long key is shortened in its path; the other never reaches a field.
	FString Text = TEXT("{\"scores\":{\"");
	Text += FString::ChrN(1 << 20, TEXT('k'));
	Text += TEXT("\":\"x\"");
	for (int32 i = 0; i < 10000; ++i)
	{
		Text += FString::Printf(TEXT(",\"k%d\":\"x\""), i);
	}
	Text += TEXT("},\"n");
	Text += FString::ChrN(1 << 20, TEXT('_'));
	Text += TEXT("umbers\":[\"x\"]}");
	TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(FStringView(Text));
	if (!TestTrue(TEXT("parses"), Parsed.HasValue()))
	{
		return false;
	}
	const FVaRestJson Input = Parsed.StealValue();

	FVaRestJsonTestAll All;
	All.Numbers = { 7 };
	FVaRestJsonDecodeReport Report;
	TestTrue(TEXT("a lenient decode succeeds"), FVaRestJsonConvert::JsonToStruct(Input, FVaRestJsonTestAll::StaticStruct(), &All, FVaRestJsonConvertOptions(), &Report));
	TestEqual(TEXT("100 issues kept, plus one entry for the rest"), Report.Warnings.Num(), 101);
	TestTrue(TEXT("... which counts them"), Report.Warnings.Num() == 101 && Report.Warnings.Last().Message.Contains(TEXT("9901 more issue(s) not recorded")));
	TestTrue(TEXT("a long key is shortened in a path"), Report.Warnings.Num() > 0 && Report.Warnings[0].Path.StartsWith(TEXT("scores.kkk"), ESearchCase::CaseSensitive)
		&& Report.Warnings[0].Path.EndsWith(TEXT("..."), ESearchCase::CaseSensitive) && Report.Warnings[0].Path.Len() <= 7 + 256 + 3);
	TestEqual(TEXT("no bad value got in"), All.Scores.Num(), 0);
	TestTrue(TEXT("a long key never matches a field through normalisation"), All.Numbers == TArray<int32>({ 7 }));

	FVaRestJsonTestAll Strict;
	FVaRestJsonDecodeReport StrictReport;
	FVaRestJsonConvertOptions StrictOptions;
	StrictOptions.bStrict = true;
	TestFalse(TEXT("strict fails"), FVaRestJsonConvert::JsonToStruct(Input, FVaRestJsonTestAll::StaticStruct(), &Strict, StrictOptions, &StrictReport));
	TestTrue(TEXT("strict: the same cap, the closing entry among the errors"), StrictReport.Errors.Num() == 101 && StrictReport.Warnings.Num() == 0
		&& StrictReport.Errors.Last().Message.Contains(TEXT("9901 more issue(s) not recorded")));

	// Without a report: one line with the totals
	AddExpectedMessagePlain(TEXT("decoding VaRestJsonTestAll: 10001 warning(s), 0 error(s); first at 'scores.kkk"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FVaRestJsonTestAll Quiet;
	FVaRestJsonConvert::JsonToStruct(Input, FVaRestJsonTestAll::StaticStruct(), &Quiet);
	return true;
}

#endif
