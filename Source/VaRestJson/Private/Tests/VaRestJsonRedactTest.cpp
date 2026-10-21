// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJsonRedact.h"

namespace VaRest::Json::RedactTestPrivate
{
	FString Redact(FUtf8StringView Input, bool bWhole, FVaRestRedactResult& OutResult,
		EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed, const FVaRestRedactRules& Rules = FVaRestRedactRules())
	{
		TArray<uint8> Out;
		OutResult = VaRest::Json::RedactPrefix(Input, Rules, Format, Out, bWhole);
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Out.GetData()), Out.Num());
	}

	FUtf8StringView V(const char* Text)
	{
		return FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRedactKeysTest, "VaRest.Json.Redact.Keys",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonRedactKeysTest::RunTest(const FString& Parameters)
{
	using K = EVaRestRedactKeyKind;
	const FVaRestRedactRules Rules;

	// Inspector spec 2.2 layer 1: lower-cased, '_' '-' '.' removed, then exact names or contained substrings.
	// VaRest's own default output is camelCase (core spec 2.4), so accessToken/clientSecret must be caught.
	const TCHAR* JsonSecrets[] = { TEXT("password"), TEXT("Password"), TEXT("user_password"), TEXT("passwd"), TEXT("accessToken"),
		TEXT("refresh-token"), TEXT("refresh_token"), TEXT("clientSecret"), TEXT("idToken"), TEXT("apiKey"), TEXT("api_key"),
		TEXT("sessionToken"), TEXT("privateKey"), TEXT("jwt"), TEXT("sig"), TEXT("key"), TEXT("KEY"), TEXT("credentials"), TEXT("signature") };
	for (const TCHAR* Key : JsonSecrets)
	{
		TestTrue(FString::Printf(TEXT("JSON key %s is a secret"), Key), Rules.ShouldRedact(FStringView(Key), K::Json));
	}
	const TCHAR* HeaderSecrets[] = { TEXT("Authorization"), TEXT("Proxy-Authorization"), TEXT("Cookie"), TEXT("Set-Cookie"),
		TEXT("X-Api-Key"), TEXT("X-CSRF-Token"), TEXT("X-Session-ID") };
	for (const TCHAR* Key : HeaderSecrets)
	{
		TestTrue(FString::Printf(TEXT("header %s is a secret"), Key), Rules.ShouldRedact(FStringView(Key), K::Header));
	}
	const TCHAR* QuerySecrets[] = { TEXT("token"), TEXT("access_token"), TEXT("X-Amz-Signature"), TEXT("X-Amz-Credential"),
		TEXT("X-Amz-Security-Token"), TEXT("X-Goog-Signature"), TEXT("sig"), TEXT("code") };
	for (const TCHAR* Key : QuerySecrets)
	{
		TestTrue(FString::Printf(TEXT("query key %s is a secret"), Key), Rules.ShouldRedact(FStringView(Key), K::Query));
	}
	const TCHAR* Plain[] = { TEXT("Idempotency-Key"), TEXT("idempotency_key"), TEXT("token_type"), TEXT("tokenType"), TEXT("username"),
		TEXT("id"), TEXT("keyboard"), TEXT("monkey"), TEXT("name"), TEXT("Content-Type") };
	for (const TCHAR* Key : Plain)
	{
		TestFalse(FString::Printf(TEXT("%s is not a secret (JSON)"), Key), Rules.ShouldRedact(FStringView(Key), K::Json));
		TestFalse(FString::Printf(TEXT("%s is not a secret (header)"), Key), Rules.ShouldRedact(FStringView(Key), K::Header));
	}
	TestFalse(TEXT("code is a secret only as a query key (OAuth authorization codes)"), Rules.ShouldRedact(FStringView(TEXT("code")), K::Json));
	TestTrue(TEXT("the substring rule over-redacts on purpose (Inspector ruling R1's stated cost)"), Rules.ShouldRedact(FStringView(TEXT("sessionCount")), K::Json));
	TestTrue(TEXT("UTF-8 keys normalise the same way"), Rules.ShouldRedact(VaRest::Json::RedactTestPrivate::V("ACCESS.TOKEN"), K::Json));

	FVaRestRedactRules Added;
	Added.AddKey(TEXT("player_pin"));
	TestTrue(TEXT("an added key matches after normalisation"), Added.ShouldRedact(FStringView(TEXT("playerPin")), K::Form));
	TestFalse(TEXT("an added key matches exactly, not as a substring"), Added.ShouldRedact(FStringView(TEXT("pin")), K::Form));
	TestFalse(TEXT("... nor inside a longer key"), Added.ShouldRedact(FStringView(TEXT("playerPinHint")), K::Form));
	TestTrue(TEXT("the built-in rules still apply"), Added.ShouldRedact(FStringView(TEXT("password")), K::Form));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRedactWholeTest, "VaRest.Json.Redact.WholeInput",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonRedactWholeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::RedactTestPrivate;
	FVaRestRedactResult R;

	const FString Nested = Redact(V(R"({"user":{"name":"neo","password":"canary-1"},"items":[{"token":{"a":[1,2]}},{"keep":true}],"n":5})"), true, R);
	TestEqualSensitive(TEXT("a secret's value of any type becomes \"***\""), Nested,
		FString(TEXT(R"({"user":{"name":"neo","password":"***"},"items":[{"token":"***"},{"keep":true}],"n":5})")));
	TestTrue(TEXT("... reported as redacted"), R.bRedacted);
	TestTrue(TEXT("... and complete"), R.StopReason == EVaRestRedactStop::Complete);

	const FString Numbers = Redact(V(R"({"big":9007199254740993,"f":1.50e+3,"neg":-0})"), true, R);
	TestEqualSensitive(TEXT("numbers are copied as written (raw numbers, no double round trip)"), Numbers, FString(TEXT(R"({"big":9007199254740993,"f":1.50e+3,"neg":-0})")));
	TestFalse(TEXT("nothing to redact"), R.bRedacted);

	const FString Escaped = Redact(V(R"({"password":"x","t":"a\"b\\c\n"})"), true, R);
	TestEqualSensitive(TEXT("strings are re-escaped"), Escaped, FString(TEXT(R"({"password":"***","t":"a\"b\\c\n"})")));
	// RapidJSON unescapes a key before handler.Key (reader.h:1005-1067): an escaped spelling must still match
	const FString EscapedKey = Redact(V(R"({"pass\u0077ord":"x","api\u005fkey":"y"})"), true, R);
	TestEqualSensitive(TEXT("keys are matched unescaped"), EscapedKey, FString(TEXT(R"({"password":"***","api_key":"***"})")));

	TestEqualSensitive(TEXT("a whole input's final number is complete"), Redact(V("42"), true, R), FString(TEXT("42")));
	TestTrue(TEXT("... Complete"), R.StopReason == EVaRestRedactStop::Complete);

	// The parser skips a UTF-8 BOM (VaRestJsonReader.cpp:39-42); the excerpt does too (review M5)
	TestEqualSensitive(TEXT("a leading BOM is skipped"), Redact(V("\xEF\xBB\xBF{\"password\":\"x\"}"), true, R), FString(TEXT(R"({"password":"***"})")));
	TestTrue(TEXT("... and the excerpt is complete"), R.StopReason == EVaRestRedactStop::Complete);

	const FString Pretty = Redact(V(R"({"a":[1,2],"password":"x"})"), true, R, EVaRestJsonFormat::Pretty);
	TestEqualSensitive(TEXT("pretty: two-space indent, like ToString"), Pretty,
		FString(TEXT("{\n  \"a\": [\n    1,\n    2\n  ],\n  \"password\": \"***\"\n}")));

	const FString Trailing = Redact(V(R"({"a":1,})"), true, R);
	TestEqualSensitive(TEXT("a parse error ends at the last complete token"), Trailing, FString(TEXT(R"({"a":1)")));
	TestTrue(TEXT("... noted as a parse error"), R.StopReason == EVaRestRedactStop::ParseError && !R.Error.IsEmpty() && R.ErrorOffset > 0);

	const FString Garbage = Redact(V(R"({"a":1}x)"), true, R);
	TestEqualSensitive(TEXT("the root is kept before trailing garbage"), Garbage, FString(TEXT(R"({"a":1})")));
	TestTrue(TEXT("... which is a parse error"), R.StopReason == EVaRestRedactStop::ParseError);

	TArray<uint8> BadUtf8 = { '"', 0xC3, 0x28, '"' };
	TArray<uint8> Out;
	R = VaRest::Json::RedactPrefix(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(BadUtf8.GetData()), BadUtf8.Num()), FVaRestRedactRules(), EVaRestJsonFormat::Condensed, Out, true);
	TestTrue(TEXT("invalid UTF-8 is a parse error, never copied"), R.StopReason == EVaRestRedactStop::ParseError && Out.Num() == 0);

	FString Deep;
	for (int32 i = 0; i < 600; ++i)
	{
		Deep.AppendChar(TEXT('['));
	}
	FTCHARToUTF8 DeepUtf8(*Deep);
	Out.Reset();
	R = VaRest::Json::RedactPrefix(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(DeepUtf8.Get()), DeepUtf8.Length()), FVaRestRedactRules(), EVaRestJsonFormat::Condensed, Out, false);
	TestTrue(TEXT("nesting past MaxDepth stops with an error"), R.StopReason == EVaRestRedactStop::ParseError && R.Error.Contains(TEXT("Nesting deeper")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRedactCutTest, "VaRest.Json.Redact.CutRules",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonRedactCutTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::RedactTestPrivate;
	FVaRestRedactResult R;

	// Inspector spec 2.2: "a matching key's value of any type, complete or cut, becomes "***"; tokens cut by the cap are
	// never written, and neither is a scalar ending exactly at the cap"
	const FString InValue = Redact(V(R"({"a":1,"password":"canary-2-lo)"), false, R);
	TestEqualSensitive(TEXT("cap inside a secret's value: the value is still ***"), InValue, FString(TEXT(R"({"a":1,"password":"***")")));
	TestFalse(TEXT("... and no canary byte"), InValue.Contains(TEXT("canary")));
	TestTrue(TEXT("... stopped by the cap"), R.StopReason == EVaRestRedactStop::Cap && R.bRedacted);

	const FString InKey = Redact(V(R"({"a":1,"passw)"), false, R);
	TestEqualSensitive(TEXT("cap inside a key: the key is not written"), InKey, FString(TEXT(R"({"a":1)")));
	TestTrue(TEXT("... Cap"), R.StopReason == EVaRestRedactStop::Cap);

	const FString InNumber = Redact(V(R"({"a":123)"), false, R);
	TestEqualSensitive(TEXT("cap inside a number: the number is not written"), InNumber, FString(TEXT(R"({"a":)")));
	TestTrue(TEXT("... Cap"), R.StopReason == EVaRestRedactStop::Cap);

	const FString AtCap = Redact(V(R"({"n":42)"), false, R);
	TestEqualSensitive(TEXT("a number ending exactly at the cap may be the front of a longer one: not written"), AtCap, FString(TEXT(R"({"n":)")));
	TestEqualSensitive(TEXT("a bare number ending at the cap"), Redact(V("42"), false, R), FString());
	TestTrue(TEXT("... Cap"), R.StopReason == EVaRestRedactStop::Cap);

	const FString InString = Redact(V(R"({"a":"xy","b":"unfini)"), false, R);
	TestEqualSensitive(TEXT("a complete string is kept; a cut one is not"), InString, FString(TEXT(R"({"a":"xy","b":)")));

	const FString InLiteral = Redact(V(R"({"a":tr)"), false, R);
	TestEqualSensitive(TEXT("a cut literal is not written"), InLiteral, FString(TEXT(R"({"a":)")));
	TestTrue(TEXT("... Cap, not a parse error"), R.StopReason == EVaRestRedactStop::Cap);

	const FString InContainer = Redact(V(R"({"token":{"a":"canary-3)"), false, R);
	TestEqualSensitive(TEXT("cap inside a redacted container: ***, no canary"), InContainer, FString(TEXT(R"({"token":"***")")));
	TestTrue(TEXT("... Cap, with no error text"), R.StopReason == EVaRestRedactStop::Cap && R.Error.IsEmpty() && R.ErrorOffset == -1);

	// A cut inside an escape: RapidJSON reports these at the backslash, bytes before the end (reader.h:1011, :919, :1031)
	TestEqualSensitive(TEXT("cut inside a \\u escape"), Redact(V(R"({"a":"x\u00)"), false, R), FString(TEXT(R"({"a":)")));
	TestTrue(TEXT("... is a cut"), R.StopReason == EVaRestRedactStop::Cap && R.Error.IsEmpty() && R.ErrorOffset == -1);
	Redact(V(R"({"a":"\ud83d\ude)"), false, R);
	TestTrue(TEXT("cut inside a surrogate pair is a cut"), R.StopReason == EVaRestRedactStop::Cap && R.Error.IsEmpty());
	Redact(V(R"({"a":"\ud83d)"), false, R);
	TestTrue(TEXT("cut between the surrogates is a cut"), R.StopReason == EVaRestRedactStop::Cap);
	Redact(V(R"({"a":"x\u00)"), true, R);
	TestTrue(TEXT("the same input as a whole body is a parse error"), R.StopReason == EVaRestRedactStop::ParseError && !R.Error.IsEmpty());
	Redact(V(R"({"a":"\ud83d\ude)"), true, R);
	TestTrue(TEXT("... also inside a surrogate pair"), R.StopReason == EVaRestRedactStop::ParseError);
	Redact(V(R"({"a":1,})"), false, R);
	TestTrue(TEXT("a syntax error at the last byte of a cut input is still a parse error"), R.StopReason == EVaRestRedactStop::ParseError);

	// The colon is written only when the input holds it
	TestEqualSensitive(TEXT("cut after a key, before its colon: no colon"), Redact(V(R"({"a":1,"b")"), false, R), FString(TEXT(R"({"a":1,"b")")));
	TestEqualSensitive(TEXT("... with whitespace after the key"), Redact(V("{\"a\" "), false, R), FString(TEXT(R"({"a")")));
	TestEqualSensitive(TEXT("a missing colon is not papered over"), Redact(V(R"({"a" 1})"), true, R), FString(TEXT(R"({"a")")));
	TestTrue(TEXT("... and is a parse error"), R.StopReason == EVaRestRedactStop::ParseError);
	TestEqualSensitive(TEXT("pretty: a cut after the colon"), Redact(V(R"({"a":12)"), false, R, EVaRestJsonFormat::Pretty), FString(TEXT("{\n  \"a\": ")));

	TArray<uint8> CutChar = { '{', '"', 'a', '"', ':', '"', 0xD0 };
	TArray<uint8> Out;
	R = VaRest::Json::RedactPrefix(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(CutChar.GetData()), CutChar.Num()), FVaRestRedactRules(), EVaRestJsonFormat::Condensed, Out, false);
	TestTrue(TEXT("a cap inside a multi-byte character is a cut, not an encoding error"), R.StopReason == EVaRestRedactStop::Cap);
	TestEqualSensitive(TEXT("... and the cut character is not written"),
		FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Out.GetData()), Out.Num()), FString(TEXT(R"({"a":)")));
	return true;
}

#endif
