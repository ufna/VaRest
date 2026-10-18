// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJson.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonPathTest, "VaRest.Json.Path",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonPathTest::RunTest(const FString& Parameters)
{
	const FVaRestJson J = FVaRestJson::Parse(FStringView(TEXT(
		"{\"user\":{\"items\":[{\"name\":\"a\"},{\"name\":\"b\"}],\"a.b\":1,\"x/y\":2,\"t~\":3,\"\":4},\"m\":[[1,2],[3]]}"))).StealValue();

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

#endif
