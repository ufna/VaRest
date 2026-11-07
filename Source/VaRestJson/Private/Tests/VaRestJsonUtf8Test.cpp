// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "VaRestJson.h"
#include "VaRestJsonUtf8.h"

namespace VaRestJsonUtf8Test
{
	FUtf8StringView Bytes(const char* Text, int32 Len)
	{
		return FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text), Len);
	}

	TArray<uint8> Repair(const TArray<uint8>& In, int32& OutReplaced)
	{
		VaRest::Json::FUtf8Scratch Out;
		OutReplaced = VaRest::Json::AppendRepairedUtf8(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(In.GetData()), In.Num()), Out);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Out.GetData()), Out.Num());
	}

	TArray<uint8> Convert(const FString& In, int32& OutReplaced)
	{
		TArray<uint8> Out;
		Out.SetNumUninitialized(static_cast<int32>(VaRest::Json::Utf8Length(In)));
		const int32 Written = VaRest::Json::WriteUtf8(In, TArrayView<UTF8CHAR>(reinterpret_cast<UTF8CHAR*>(Out.GetData()), Out.Num()), OutReplaced);
		check(Written == Out.Num());
		return Out;
	}

	FString WithUnit(const TCHAR* Before, uint16 Unit, const TCHAR* After)
	{
		FString Result = Before;
		Result.AppendChar(static_cast<TCHAR>(Unit));
		Result += After;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonUtf8RepairTest, "VaRest.Json.Utf8.Repair",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonUtf8RepairTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonUtf8Test;
	struct FCase
	{
		const TCHAR* Name;
		TArray<uint8> In;
		TArray<uint8> Out;
		int32 Replaced;
	};
	// One U+FFFD per maximal subpart (The Unicode Standard, ch. 3 "U+FFFD Substitution of Maximal Subparts")
	const FCase Cases[] = {
		{ TEXT("stray continuation"), { 'a', 0x80, 'b' }, { 'a', 0xEF, 0xBF, 0xBD, 'b' }, 1 },
		{ TEXT("overlong lead C0"), { 0xC0, 0xAF }, { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD }, 2 },
		{ TEXT("overlong E0 80 80"), { 0xE0, 0x80, 0x80 }, { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD }, 3 },
		{ TEXT("truncated 3-byte"), { 0xE2, 0x82 }, { 0xEF, 0xBF, 0xBD }, 1 },
		{ TEXT("CESU surrogate"), { 0xED, 0xA0, 0x80 }, { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD }, 3 },
		{ TEXT("above U+10FFFF"), { 0xF4, 0x90, 0x80, 0x80 }, { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD }, 4 },
		{ TEXT("truncated 4-byte then ASCII"), { 0xF0, 0x9F, 0x98, 'x' }, { 0xEF, 0xBF, 0xBD, 'x' }, 1 },
		{ TEXT("minimum 3-byte scalar"), { 0xE0, 0xA0, 0x80 }, { 0xE0, 0xA0, 0x80 }, 0 },
		{ TEXT("last overlong 3-byte prefix"), { 0xE0, 0x9F, 0xBF }, { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD }, 3 },
		{ TEXT("last scalar before surrogate range"), { 0xED, 0x9F, 0xBF }, { 0xED, 0x9F, 0xBF }, 0 },
		{ TEXT("minimum 4-byte scalar"), { 0xF0, 0x90, 0x80, 0x80 }, { 0xF0, 0x90, 0x80, 0x80 }, 0 },
		{ TEXT("maximum Unicode scalar"), { 0xF4, 0x8F, 0xBF, 0xBF }, { 0xF4, 0x8F, 0xBF, 0xBF }, 0 },
		{ TEXT("overlong lead C1"), { 0xC1, 0xBF }, { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD }, 2 },
		{ TEXT("FF never starts a sequence"), { 0xFF, 0x80 }, { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD }, 2 },
		{ TEXT("overlong F0 80"), { 0xF0, 0x80, 0x80, 0x80 }, { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD }, 4 },
		{ TEXT("F5 lead"), { 0xF5, 0x80, 0x80, 0x80 }, { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD }, 4 },
		{ TEXT("F7 lead"), { 0xF7, 0xBF }, { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD }, 2 },
		{ TEXT("valid F1..F3 path"), { 0xF1, 0x80, 0x80, 0x80, 0xF3, 0xBF, 0xBF, 0xBF }, { 0xF1, 0x80, 0x80, 0x80, 0xF3, 0xBF, 0xBF, 0xBF }, 0 },
		{ TEXT("lone lead at the end"), { 'a', 0xC3 }, { 'a', 0xEF, 0xBF, 0xBD }, 1 },
		{ TEXT("valid incl. NUL"), { 0xD0, 0x9A, 0xF0, 0x9F, 0x98, 0x80, 0x00, 'z' }, { 0xD0, 0x9A, 0xF0, 0x9F, 0x98, 0x80, 0x00, 'z' }, 0 },
	};
	for (const FCase& Case : Cases)
	{
		int32 Replaced = -1;
		const TArray<uint8> Out = Repair(Case.In, Replaced);
		TestTrue(FString::Printf(TEXT("%s: bytes"), Case.Name), Out == Case.Out);
		TestEqual(FString::Printf(TEXT("%s: replacements"), Case.Name), Replaced, Case.Replaced);
		const bool bValid = VaRest::Json::IsValidUtf8(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Case.In.GetData()), Case.In.Num()));
		TestTrue(FString::Printf(TEXT("%s: IsValidUtf8"), Case.Name), bValid == (Case.Replaced == 0));
	}

	// Every F0 80..8F prefix is overlong. Its lead and three stray continuations
	// are four maximal subparts; the following ASCII byte must not be consumed.
	const TArray<uint8> FourReplacementsAndAscii = { 0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD,
		0xEF, 0xBF, 0xBD, 0xEF, 0xBF, 0xBD, 'x' };
	for (int32 Second = 0x80; Second <= 0x8F; ++Second)
	{
		const TArray<uint8> In = { 0xF0, static_cast<uint8>(Second), 0x80, 0x80, 'x' };
		int32 Replaced = -1;
		TestTrue(FString::Printf(TEXT("F0 %02X: four replacements preserve following ASCII"), Second), Repair(In, Replaced) == FourReplacementsAndAscii);
		TestEqual(FString::Printf(TEXT("F0 %02X: four maximal subparts"), Second), Replaced, 4);
		TestFalse(FString::Printf(TEXT("F0 %02X: input is invalid"), Second),
			VaRest::Json::IsValidUtf8(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(In.GetData()), In.Num())));
	}

	int32 Replaced = -1;
	TestTrue(TEXT("lone high surrogate at the end"), Convert(WithUnit(TEXT("a"), 0xD800, TEXT("")), Replaced) == TArray<uint8>({ 'a', 0xEF, 0xBF, 0xBD }) && Replaced == 1);
	TestTrue(TEXT("lone low surrogate"), Convert(WithUnit(TEXT(""), 0xDC00, TEXT("b")), Replaced) == TArray<uint8>({ 0xEF, 0xBF, 0xBD, 'b' }) && Replaced == 1);
	FString Reversed;
	Reversed.AppendChar(static_cast<TCHAR>(0xDC00));
	Reversed.AppendChar(static_cast<TCHAR>(0xD800));
	TestTrue(TEXT("reversed pair is two replacements"), Convert(Reversed, Replaced).Num() == 6 && Replaced == 2);
	FString Pair;
	Pair.AppendChar(static_cast<TCHAR>(0xD83D));
	Pair.AppendChar(static_cast<TCHAR>(0xDE00));
	TestTrue(TEXT("a real pair is one 4-byte character"), Convert(Pair, Replaced) == TArray<uint8>({ 0xF0, 0x9F, 0x98, 0x80 }) && Replaced == 0);

	// A buffer sized by the engine's ConvertedLength (a lone surrogate = 1 byte) is too small for U+FFFD (3 bytes):
	// WriteUtf8 must refuse, not overrun. A guard byte after the view proves nothing past it is touched.
	const FString Lone = WithUnit(TEXT("a"), 0xD800, TEXT("b"));
	const int32 EngineLen = FPlatformString::ConvertedLength<UTF8CHAR>(Lone.GetCharArray().GetData(), Lone.Len());
	const int32 OwnLen = static_cast<int32>(VaRest::Json::Utf8Length(Lone));
	TestEqual(TEXT("the converters disagree by 2 bytes on a lone surrogate"), OwnLen, EngineLen + 2);
	TArray<UTF8CHAR> Small;
	Small.Init(static_cast<UTF8CHAR>(0x55), EngineLen + 1); // the last byte is the guard
	int32 SmallReplaced = 0;
	TestEqual(TEXT("an engine-sized buffer is too small"), VaRest::Json::WriteUtf8(Lone, TArrayView<UTF8CHAR>(Small.GetData(), EngineLen), SmallReplaced), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("the guard byte is untouched"), static_cast<int32>(Small.Last()), 0x55);
	TArray<UTF8CHAR> Exact;
	Exact.Init(static_cast<UTF8CHAR>(0x55), OwnLen + 1);
	TestEqual(TEXT("a Utf8Length buffer is enough"), VaRest::Json::WriteUtf8(Lone, TArrayView<UTF8CHAR>(Exact.GetData(), OwnLen), SmallReplaced), OwnLen);
	TestEqual(TEXT("... and its guard byte is untouched"), static_cast<int32>(Exact.Last()), 0x55);
	TestEqual(TEXT("... with one replacement"), SmallReplaced, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonUtf8ApiTest, "VaRest.Json.Utf8.ApiStrings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonUtf8ApiTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonUtf8Test;
	// key + value of the byte Set (2), the "q" key Set (1), the two-bad-bytes value (1, one warning for two bad bytes), the FString value (1), the FString key (1), the bridge string (1); lookups store nothing and stay quiet
	AddExpectedMessagePlain(TEXT("replaced with U+FFFD"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 7);

	FVaRestJson J = FVaRestJson::Object();
	J.Set(Bytes("k\x80", 2), Bytes("v\xFF", 2));
	TestEqualSensitive(TEXT("repaired key and value"), J.ToString(), FString(TEXT("{\"k�\":\"v�\"}")));
	TestTrue(TEXT("what is written parses back"), FVaRestJson::Parse(FStringView(J.ToString())).HasValue());

	const FString Lone = WithUnit(TEXT("a"), 0xD800, TEXT(""));
	J.Set(TEXT("s"), Lone);
	TestTrue(TEXT("lone surrogate value -> U+FFFD"), J[TEXT("s")].AsUtf8View().Equals(Bytes("a\xEF\xBF\xBD", 4), ESearchCase::CaseSensitive));
	J.Set(Lone, 1);
	TestEqual(TEXT("a key with a lone surrogate is found by the same text"), J[FStringView(Lone)].AsInt(), 1);

	// Lookups repair a byte key the way Set stored it, quietly
	J.Set(Bytes("q\x80", 2), 1);
	TestEqual(TEXT("a byte key is found by the same bytes"), J[Bytes("q\x80", 2)].AsInt(), 1);
	TestTrue(TEXT("... and removed by them"), J.Remove(Bytes("q\x80", 2)));
	TestFalse(TEXT("... gone"), J.Has(Bytes("q\x80", 2)));
	// Two bad sequences in one string: still one warning
	J.Set(Bytes("two", 3), Bytes("k\x80\x80", 3));
	TestTrue(TEXT("both bad bytes repaired"), J[TEXT("two")].AsUtf8View().Equals(Bytes("k\xEF\xBF\xBD\xEF\xBF\xBD", 7), ESearchCase::CaseSensitive));

	J.Set(TEXT("ansi"), "plain");
	TestEqualSensitive(TEXT("an ANSI literal is a UTF-8 string value"), J[TEXT("ansi")].AsString(), FString(TEXT("plain")));

	const FVaRestJson Bridged = FVaRestJson::FromJsonValue(MakeShared<FJsonValueString>(Lone));
	TestTrue(TEXT("bridge strings are converted the same way"), Bridged.AsUtf8View().Equals(Bytes("a\xEF\xBF\xBD", 4), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonUtf8ColumnsTest, "VaRest.Json.Utf8.ParseAndColumns",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonUtf8ColumnsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonUtf8Test;
	// The two FString parses with an unpaired surrogate warn once each
	AddExpectedMessagePlain(TEXT("replaced with U+FFFD"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);

	// One converter for the parse and its diagnostics: an unpaired surrogate is U+FFFD (3 bytes) in both
	auto LoneResult = FVaRestJson::Parse(FStringView(WithUnit(TEXT("[\""), 0xD800, TEXT("\",x]"))));
	auto FffdResult = FVaRestJson::Parse(FStringView(TEXT("[\"�\",x]")));
	if (!TestFalse(TEXT("malformed input fails (lone surrogate)"), LoneResult.HasValue()) || !TestFalse(TEXT("malformed input fails (U+FFFD)"), FffdResult.HasValue()))
	{
		return false;
	}
	const FVaRestJsonError Lone = LoneResult.GetError();
	const FVaRestJsonError Fffd = FffdResult.GetError();
	TestEqual(TEXT("same offset as a real U+FFFD"), Lone.Offset, Fffd.Offset);
	TestEqual(TEXT("same column as a real U+FFFD"), Lone.Column, Fffd.Column);

	auto ParsedResult = FVaRestJson::Parse(FStringView(WithUnit(TEXT("{\"s\":\""), 0xD800, TEXT("\"}"))));
	if (!TestTrue(TEXT("a lone surrogate inside a string parses"), ParsedResult.HasValue()))
	{
		return false;
	}
	const FVaRestJson Parsed = ParsedResult.StealValue();
	TestTrue(TEXT("a lone surrogate inside a string parses as U+FFFD"), Parsed[TEXT("s")].AsUtf8View().Equals(Bytes("\xEF\xBF\xBD", 3), ESearchCase::CaseSensitive));

	// Columns count code points; a BOM is not a column (Offset stays in bytes)
	auto AsciiResult = FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("{\"e\":x}")));
	auto AccentedResult = FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("{\"é\":x}")));
	TArray<uint8> WithBom = { 0xEF, 0xBB, 0xBF, '{', '"', 'e', '"', ':', 'x', '}' };
	auto BomResult = FVaRestJson::Parse(TConstArrayView<uint8>(WithBom));
	if (!TestFalse(TEXT("malformed ASCII fails"), AsciiResult.HasValue()) || !TestFalse(TEXT("malformed accented fails"), AccentedResult.HasValue())
		|| !TestFalse(TEXT("malformed BOM input fails"), BomResult.HasValue()))
	{
		return false;
	}
	const FVaRestJsonError Ascii = AsciiResult.GetError();
	const FVaRestJsonError Accented = AccentedResult.GetError();
	TestEqual(TEXT("a two-byte character is one column"), Accented.Column, Ascii.Column);
	TestEqual(TEXT("... and two bytes"), Accented.Offset, Ascii.Offset + 1);
	const FVaRestJsonError Bom = BomResult.GetError();
	TestEqual(TEXT("the BOM is not a column"), Bom.Column, Ascii.Column);
	TestEqual(TEXT("the BOM is three bytes"), Bom.Offset, Ascii.Offset + 3);
	TestEqual(TEXT("line 1"), Bom.Line, 1);
	return true;
}

#endif
