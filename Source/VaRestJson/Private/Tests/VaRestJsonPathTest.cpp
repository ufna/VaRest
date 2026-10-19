// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJson.h"

namespace VaRestJsonPathTest
{
	FVaRestJson Fixture()
	{
		return FVaRestJson::Parse(FStringView(TEXT(
			"{\"user\":{\"items\":[{\"name\":\"a\"},{\"name\":\"b\"}],\"a.b\":1,\"x/y\":2,\"t~\":3,\"\":4},\"m\":[[1,2],[3]],"
			"\"~1\":5,\"x~2\":7,\"x~\":8,\"s\":\"str\",\"a\":[1]}"))).StealValue();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonPathTest, "VaRest.Json.Path.Access",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonPathTest::RunTest(const FString& Parameters)
{
	const FVaRestJson J = VaRestJsonPathTest::Fixture();

	TestEqualSensitive(TEXT("dotted"), J.Path(TEXT("user.items[1].name")).AsString(), FString(TEXT("b")));
	TestEqual(TEXT("nested brackets"), J.Path(TEXT("m[0][1]")).AsInt(), 2);
	TestEqual(TEXT("root index"), FVaRestJson::Array(FVaRestJson::Object().Set(TEXT("q"), 9)).Path(TEXT("[0].q")).AsInt(), 9);
	TestTrue(TEXT("empty path is self"), J.Path(TEXT("")).IsSameNode(J));

	TestEqualSensitive(TEXT("pointer"), J.Path(TEXT("/user/items/0/name")).AsString(), FString(TEXT("a")));
	TestEqual(TEXT("pointer: key with a dot"), J.Path(TEXT("/user/a.b")).AsInt(), 1);
	TestEqual(TEXT("pointer: ~1 is /"), J.Path(TEXT("/user/x~1y")).AsInt(), 2);
	TestEqual(TEXT("pointer: ~0 is ~"), J.Path(TEXT("/user/t~0")).AsInt(), 3);
	TestEqual(TEXT("pointer: empty key"), J.Path(TEXT("/user/")).AsInt(), 4);

	TestFalse(TEXT("miss"), J.Path(TEXT("user.nope")).IsValid());
	TestFalse(TEXT("index out of range"), J.Path(TEXT("user.items[5]")).IsValid());
	TestFalse(TEXT("syntax: double dot"), J.Path(TEXT("user..items")).IsValid());
	TestFalse(TEXT("syntax: open bracket"), J.Path(TEXT("m[0")).IsValid());
	TestFalse(TEXT("syntax: non-digit index"), J.Path(TEXT("m[x]")).IsValid());
	TestFalse(TEXT("syntax: trailing dot"), J.Path(TEXT("user.")).IsValid());
	TestFalse(TEXT("pointer: leading zero index"), J.Path(TEXT("/m/01")).IsValid());
	TestFalse(TEXT("index on object"), J.Path(TEXT("user[0]")).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonPathMissTest, "VaRest.Json.Path.MissKeepsDocument",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonPathMissTest::RunTest(const FString& Parameters)
{
	const FVaRestJson J = VaRestJsonPathTest::Fixture();
	const FString Before = J.ToString();

	// Like Get/At: a missed Path keeps the document, so a write through it is refused loudly
	// instead of auto-vivifying a detached object that nobody will ever see.
	AddExpectedError(TEXT("cannot Set a field on a missing value"), EAutomationExpectedErrorFlags::Contains, 2);
	J.Path(TEXT("user.settings")).Set(TEXT("x"), 1);
	J.Path(TEXT("/user/settings")).Set(TEXT("x"), 1);
	TestEqualSensitive(TEXT("J unchanged"), J.ToString(), Before);

	for (const TCHAR* Expression : { TEXT("user.settings"), TEXT("/user/settings"), TEXT("user..items"), TEXT("m[x]"), TEXT("/x~2"), TEXT("s.x"), TEXT("/s/x") })
	{
		const FVaRestJson Miss = J.Path(Expression);
		TestFalse(*FString::Printf(TEXT("%s is a miss"), Expression), Miss.IsValid());
		TestTrue(*FString::Printf(TEXT("%s keeps the document"), Expression), Miss.GetDocumentInternal() == J.GetDocumentInternal());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonPathEdgesTest, "VaRest.Json.Path.Edges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonPathEdgesTest::RunTest(const FString& Parameters)
{
	const FVaRestJson J = VaRestJsonPathTest::Fixture();

	// RFC 6901 §4: unescaping "~01" must give "~1" (~1 -> '/' first would give "~/")
	TestEqual(TEXT("/~01 reaches key ~1"), J.Path(TEXT("/~01")).AsInt(), 5);
	// "~" followed by anything but 0 or 1 is not a valid pointer, even when a key spells it literally
	TestFalse(TEXT("/x~2 is invalid"), J.Path(TEXT("/x~2")).IsValid());
	TestFalse(TEXT("/x~ is invalid"), J.Path(TEXT("/x~")).IsValid());

	// Array indices
	TestFalse(TEXT("/m/- (past the end) is not readable"), J.Path(TEXT("/m/-")).IsValid());
	TestFalse(TEXT("m[9999999999] does not overflow"), J.Path(TEXT("m[9999999999]")).IsValid());
	TestFalse(TEXT("/m/4294967296 does not wrap to 0"), J.Path(TEXT("/m/4294967296")).IsValid());

	// Keys are case-sensitive
	TestFalse(TEXT("User.items"), J.Path(TEXT("User.items")).IsValid());
	TestFalse(TEXT("/USER"), J.Path(TEXT("/USER")).IsValid());

	// Through a scalar, and a dotted name that only exists as one key with a dot
	TestFalse(TEXT("user.a.b.c"), J.Path(TEXT("user.a.b.c")).IsValid());
	TestFalse(TEXT("s.x"), J.Path(TEXT("s.x")).IsValid());
	TestFalse(TEXT("s[0]"), J.Path(TEXT("s[0]")).IsValid());
	TestFalse(TEXT("m[0][0].x"), J.Path(TEXT("m[0][0].x")).IsValid());
	TestFalse(TEXT("/m/0/0/x"), J.Path(TEXT("/m/0/0/x")).IsValid());

	// On an invalid handle
	TestFalse(TEXT("default handle"), FVaRestJson().Path(TEXT("a")).IsValid());
	TestFalse(TEXT("default handle, pointer"), FVaRestJson().Path(TEXT("/a")).IsValid());
	TestFalse(TEXT("missed handle"), J[TEXT("nope")].Path(TEXT("a")).IsValid());

	// Dotted syntax errors on names that do exist
	TestFalse(TEXT(".a"), J.Path(TEXT(".a")).IsValid());
	TestFalse(TEXT("a[]"), J.Path(TEXT("a[]")).IsValid());
	TestFalse(TEXT("a]"), J.Path(TEXT("a]")).IsValid());
	TestEqual(TEXT("a[0] still works"), J.Path(TEXT("a[0]")).AsInt(), 1);
	return true;
}

#endif
