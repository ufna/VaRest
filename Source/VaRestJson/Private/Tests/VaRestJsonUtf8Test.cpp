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
		OutReplaced = VaRest::Json::WriteUtf8(In, reinterpret_cast<UTF8CHAR*>(Out.GetData()));
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
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonUtf8ApiTest, "VaRest.Json.Utf8.ApiStrings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonUtf8ApiTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonUtf8Test;
	// key + value of the byte Set (2), the FString value (1), the FString key (1), the bridge string (1); lookups store nothing and stay quiet
	AddExpectedMessagePlain(TEXT("replaced with U+FFFD"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 5);

	FVaRestJson J = FVaRestJson::Object();
	J.Set(Bytes("k\x80", 2), Bytes("v\xFF", 2));
	TestEqualSensitive(TEXT("repaired key and value"), J.ToString(), FString(TEXT("{\"k�\":\"v�\"}")));
	TestTrue(TEXT("what is written parses back"), FVaRestJson::Parse(FStringView(J.ToString())).HasValue());

	const FString Lone = WithUnit(TEXT("a"), 0xD800, TEXT(""));
	J.Set(TEXT("s"), Lone);
	TestTrue(TEXT("lone surrogate value -> U+FFFD"), J[TEXT("s")].AsUtf8View().Equals(Bytes("a\xEF\xBF\xBD", 4), ESearchCase::CaseSensitive));
	J.Set(Lone, 1);
	TestEqual(TEXT("a key with a lone surrogate is found by the same text"), J[FStringView(Lone)].AsInt(), 1);

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
	const FVaRestJsonError Lone = FVaRestJson::Parse(FStringView(WithUnit(TEXT("[\""), 0xD800, TEXT("\",x]")))).GetError();
	const FVaRestJsonError Fffd = FVaRestJson::Parse(FStringView(TEXT("[\"�\",x]"))).GetError();
	TestEqual(TEXT("same offset as a real U+FFFD"), Lone.Offset, Fffd.Offset);
	TestEqual(TEXT("same column as a real U+FFFD"), Lone.Column, Fffd.Column);

	const FVaRestJson Parsed = FVaRestJson::Parse(FStringView(WithUnit(TEXT("{\"s\":\""), 0xD800, TEXT("\"}")))).StealValue();
	TestTrue(TEXT("a lone surrogate inside a string parses as U+FFFD"), Parsed[TEXT("s")].AsUtf8View().Equals(Bytes("\xEF\xBF\xBD", 3), ESearchCase::CaseSensitive));

	// Columns count code points; a BOM is not a column (Offset stays in bytes)
	const FVaRestJsonError Ascii = FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("{\"e\":x}"))).GetError();
	const FVaRestJsonError Accented = FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("{\"é\":x}"))).GetError();
	TestEqual(TEXT("a two-byte character is one column"), Accented.Column, Ascii.Column);
	TestEqual(TEXT("... and two bytes"), Accented.Offset, Ascii.Offset + 1);
	TArray<uint8> WithBom = { 0xEF, 0xBB, 0xBF, '{', '"', 'e', '"', ':', 'x', '}' };
	const FVaRestJsonError Bom = FVaRestJson::Parse(TConstArrayView<uint8>(WithBom)).GetError();
	TestEqual(TEXT("the BOM is not a column"), Bom.Column, Ascii.Column);
	TestEqual(TEXT("the BOM is three bytes"), Bom.Offset, Ascii.Offset + 3);
	TestEqual(TEXT("line 1"), Bom.Line, 1);
	return true;
}

#endif
