// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJson.h"
#include "VaRestJsonDocument.h"

namespace VaRestJsonSemanticsTest
{
	FVaRestJson ParseText(const UTF8CHAR* Text)
	{
		return FVaRestJson::Parse(FUtf8StringView(Text)).StealValue();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonMissingIsUndefinedTest, "VaRest.Json.Semantics.MissingIsUndefined",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonMissingIsUndefinedTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonSemanticsTest;
	const FVaRestJson Resp = ParseText(UTF8TEXT("{\"q\":1}"));

	FVaRestJson Body = FVaRestJson::Object();
	Body.Set(TEXT("p"), Resp[TEXT("p")]);
	TestFalse(TEXT("a missed lookup is not written (forwarding an absent field)"), Body.Has(TEXT("p")));
	Body.Set(TEXT("q"), Resp[TEXT("q")]);
	TestEqual(TEXT("a present value is copied"), Body[TEXT("q")].AsInt(), 1);
	Body.Set(TEXT("q"), FVaRestJson());
	TestFalse(TEXT("Set with a default handle removes the key"), Body.Has(TEXT("q")));
	Body.Set(UTF8TEXT("u"), 1).Set(FUtf8StringView(UTF8TEXT("u")), Resp[TEXT("nope")]);
	TestFalse(TEXT("the UTF-8 key overload removes too"), Body.Has(TEXT("u")));
	TestEqualSensitive(TEXT("no undefined key reaches the text"), Body.ToString(), FString(TEXT("{}")));

	FVaRestJson Fresh;
	Fresh.Set(TEXT("k"), Fresh);
	TestTrue(TEXT("Fresh.Set(K, Fresh) is an empty object"), Fresh.IsObject() && Fresh.Num() == 0);

	Body.Set(TEXT("n"), FVaRestJson::Null()).Set(TEXT("m"), nullptr);
	TestTrue(TEXT("explicit null is written"), Body[TEXT("n")].IsNull() && Body[TEXT("m")].IsNull());

	FVaRestJson List = FVaRestJson::Array();
	List.Add(Resp[TEXT("nope")]).Add(1);
	List.SetAt(1, FVaRestJson());
	TestEqualSensitive(TEXT("Add/SetAt write null, like JSON.stringify([undefined])"), List.ToString(), FString(TEXT("[null,null]")));

	AddExpectedErrorPlain(TEXT("cannot Set a field on a missing value"), EAutomationExpectedErrorFlags::Contains, 1);
	FVaRestJson Missed = Resp[TEXT("nope")];
	Missed.Set(TEXT("k"), Missed);
	TestFalse(TEXT("a missed lookup still refuses writes"), Missed.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonSetCollapsesTest, "VaRest.Json.Semantics.SetCollapsesDuplicates",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonSetCollapsesTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonSemanticsTest;
	FVaRestJson Small = ParseText(UTF8TEXT("{\"a\":1,\"b\":2,\"a\":3}"));
	TestTrue(TEXT("parse records duplicates"), Small.GetNodeInternal()->bDuplicateKeys);
	TestEqual(TEXT("lookup sees the last"), Small[TEXT("a")].AsInt(), 3);
	Small.Set(TEXT("a"), 5);
	TestEqualSensitive(TEXT("one a, where the first one was"), Small.ToString(), FString(TEXT("{\"a\":5,\"b\":2}")));
	TestEqual(TEXT("Num"), Small.Num(), 2);

	const FVaRestJson Clean = ParseText(UTF8TEXT("{\"a\":1,\"b\":2}"));
	TestFalse(TEXT("no duplicates, no flag: Set pays nothing extra"), Clean.GetNodeInternal()->bDuplicateKeys);

	FVaRestJson Wide = ParseText(UTF8TEXT("{\"k\":1,\"a\":0,\"b\":0,\"c\":0,\"d\":0,\"e\":0,\"f\":0,\"g\":0,\"h\":0,\"k\":2,\"z\":9}"));
	TestTrue(TEXT("indexed parse records duplicates"), Wide.GetNodeInternal()->bDuplicateKeys);
	Wide.Set(TEXT("k"), 7);
	TestEqual(TEXT("one k left"), Wide.Num(), 10);
	TestEqual(TEXT("k"), Wide[TEXT("k")].AsInt(), 7);
	TestEqual(TEXT("other keys are still found after the reindex"), Wide[TEXT("z")].AsInt(), 9);
	TestTrue(TEXT("k kept the first position"), Wide.ToString().StartsWith(TEXT("{\"k\":7,\"a\":0"), ESearchCase::CaseSensitive));

	FVaRestJson Copy = ParseText(UTF8TEXT("{\"x\":1,\"x\":2}")).Clone();
	TestEqualSensitive(TEXT("Clone keeps the duplicates"), Copy.ToString(), FString(TEXT("{\"x\":1,\"x\":2}")));
	Copy.Set(TEXT("x"), 3);
	TestEqualSensitive(TEXT("Set on the clone collapses them"), Copy.ToString(), FString(TEXT("{\"x\":3}")));

	FVaRestJson Nested = ParseText(UTF8TEXT("{\"o\":{\"y\":1,\"y\":2}}"));
	Nested[TEXT("o")].Set(TEXT("y"), 0);
	TestEqualSensitive(TEXT("a nested object collapses"), Nested.ToString(), FString(TEXT("{\"o\":{\"y\":0}}")));
	return true;
}

#endif
