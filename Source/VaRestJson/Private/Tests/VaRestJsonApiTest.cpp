// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/ParallelFor.h"
#include "Misc/AutomationTest.h"
#include "VaRestJson.h"
#include "VaRestJsonDocument.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonApiBuildReadTest, "VaRest.Json.Api.BuildAndRead",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonApiBuildReadTest::RunTest(const FString& Parameters)
{
	FVaRestJson J = FVaRestJson::Object()
		.Set(TEXT("name"), TEXT("Neo"))
		.Set(TEXT("level"), 42)
		.Set(TEXT("ratio"), 0.5)
		.Set(TEXT("alive"), true)
		.Set(TEXT("nothing"), nullptr)
		.Set(TEXT("tags"), FVaRestJson::Array(TEXT("a"), TEXT("b"), 3));

	TestTrue(TEXT("type"), J.GetType() == EVaRestJsonType::Object);
	TestEqual(TEXT("Num"), J.Num(), 6);
	TestEqual(TEXT("string"), J[TEXT("name")].AsString(), FString(TEXT("Neo")));
	TestEqual(TEXT("int"), J[TEXT("level")].AsInt(), 42);
	TestEqual(TEXT("double"), J[TEXT("ratio")].AsDouble(), 0.5);
	TestTrue(TEXT("bool"), J[TEXT("alive")].AsBool());
	TestTrue(TEXT("null"), J[TEXT("nothing")].IsNull());
	TestEqual(TEXT("array len"), J[TEXT("tags")].Num(), 3);
	TestEqual(TEXT("array elem"), J[TEXT("tags")][1].AsString(), FString(TEXT("b")));
	TestEqual(TEXT("utf8 key"), J[UTF8TEXT("level")].AsInt(), 42);

	// Missing keys and wrong types give the default, never a crash
	TestFalse(TEXT("missing is invalid"), J[TEXT("nope")].IsValid());
	TestEqual(TEXT("missing -> default"), J[TEXT("nope")].AsInt(7), 7);
	TestEqual(TEXT("wrong type -> default"), J[TEXT("name")].AsInt(-1), -1);
	TestTrue(TEXT("index on object -> invalid"), J[0].GetType() == EVaRestJsonType::None);
	TestEqual(TEXT("chained on missing"), J[TEXT("a")][TEXT("b")][3].AsString(TEXT("d")), FString(TEXT("d")));

	// FName / FText / FString / UTF-8 values
	J.Set(TEXT("n"), FName(TEXT("Nm"))).Set(TEXT("t"), FText::FromString(TEXT("Tx"))).Set(TEXT("s"), FString(TEXT("St"))).Set(TEXT("u"), FUtf8StringView(UTF8TEXT("Ут")));
	TestEqual(TEXT("FName"), J[TEXT("n")].AsString(), FString(TEXT("Nm")));
	TestEqual(TEXT("FText"), J[TEXT("t")].AsString(), FString(TEXT("Tx")));
	TestEqual(TEXT("FString"), J[TEXT("s")].AsString(), FString(TEXT("St")));
	TestEqual(TEXT("UTF-8"), J[TEXT("u")].AsString(), FString(TEXT("Ут")));
	TestTrue(TEXT("zero-copy view"), J[TEXT("u")].AsUtf8View().Equals(FUtf8StringView(UTF8TEXT("Ут")), ESearchCase::CaseSensitive));
	J.Set(UTF8TEXT("raw"), 5);
	TestTrue(TEXT("UTF-8 key Set/Has"), J.Has(FUtf8StringView(UTF8TEXT("raw"))) && J[TEXT("raw")].AsInt() == 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonApiTryGetTest, "VaRest.Json.Api.TryGetRanges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonApiTryGetTest::RunTest(const FString& Parameters)
{
	FVaRestJson J = FVaRestJson::Object()
		.Set(TEXT("big"), 300)
		.Set(TEXT("neg"), -1)
		.Set(TEXT("whole"), 3.0)
		.Set(TEXT("frac"), 3.5)
		.Set(TEXT("max64"), MAX_int64)
		.Set(TEXT("umax"), MAX_uint64);

	TestFalse(TEXT("300 is not an int8"), J[TEXT("big")].TryGet<int8>().IsSet());
	TestEqual(TEXT("300 is an int16"), (int32)J[TEXT("big")].TryGet<int16>().Get(0), 300);
	TestFalse(TEXT("-1 is not a uint32"), J[TEXT("neg")].TryGet<uint32>().IsSet());
	TestEqual(TEXT("3.0 reads as int"), J[TEXT("whole")].TryGet<int32>().Get(0), 3);
	TestFalse(TEXT("3.5 does not read as int"), J[TEXT("frac")].TryGet<int32>().IsSet());
	TestEqual(TEXT("int64 max"), J[TEXT("max64")].AsInt64(), MAX_int64);
	TestFalse(TEXT("uint64 max is not an int64"), J[TEXT("umax")].TryGet<int64>().IsSet());
	TestEqual(TEXT("uint64 max"), J[TEXT("umax")].AsUInt64(), MAX_uint64);
	TestEqual(TEXT("float from int"), J[TEXT("big")].AsFloat(), 300.f);
	TestFalse(TEXT("no string coercion"), J[TEXT("big")].TryGet<FString>().IsSet());
	TestFalse(TEXT("no bool coercion"), J[TEXT("big")].TryGet<bool>().IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonApiSemanticsTest, "VaRest.Json.Api.ReferenceSemantics",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonApiSemanticsTest::RunTest(const FString& Parameters)
{
	FVaRestJson Root = FVaRestJson::Object().Set(TEXT("user"), FVaRestJson::Object());

	// A handle into the tree aliases the node: mutation is visible through the parent
	FVaRestJson User = Root[TEXT("user")];
	User.Set(TEXT("x"), 1);
	TestEqual(TEXT("mutation through child handle"), Root[TEXT("user")][TEXT("x")].AsInt(), 1);

	// Set copies its value: later changes to the source do not reach the target
	FVaRestJson Temp = FVaRestJson::Object();
	Root.Set(TEXT("copy"), Temp);
	Temp.Set(TEXT("y"), 2);
	TestFalse(TEXT("Set copied"), Root[TEXT("copy")].Has(TEXT("y")));

	// Setting a tree into itself copies the tree as it was
	Root.Set(TEXT("self"), Root);
	TestTrue(TEXT("self copy has user"), Root[TEXT("self")][TEXT("user")].IsObject());
	TestFalse(TEXT("self copy predates itself"), Root[TEXT("self")].Has(TEXT("self")));

	// Clone is independent
	FVaRestJson Clone = Root.Clone();
	Clone[TEXT("user")].Set(TEXT("x"), 100);
	TestEqual(TEXT("clone does not alias"), Root[TEXT("user")][TEXT("x")].AsInt(), 1);
	TestTrue(TEXT("clone of unchanged subtree equals"), Clone[TEXT("copy")].Equals(Root[TEXT("copy")]));

	// A child handle keeps the whole document alive
	FVaRestJson Survivor;
	{
		FVaRestJson Scoped = FVaRestJson::Object().Set(TEXT("k"), TEXT("v"));
		Survivor = Scoped[TEXT("k")];
	}
	TestEqual(TEXT("child outlives its root handle"), Survivor.AsString(), FString(TEXT("v")));

	// Auto-vivify: an invalid handle becomes the container the first write needs
	FVaRestJson Fresh;
	Fresh.Set(TEXT("a"), 1);
	TestTrue(TEXT("Set on invalid -> object"), Fresh.IsObject());
	FVaRestJson FreshArray;
	FreshArray.Add(1);
	TestTrue(TEXT("Add on invalid -> array"), FreshArray.IsArray());

	// Writing into the wrong type is an error and a no-op
	AddExpectedError(TEXT("FVaRestJson: cannot Set a field on a String"), EAutomationExpectedErrorFlags::Contains, 1);
	FVaRestJson Str = FVaRestJson::From(TEXT("s"));
	Str.Set(TEXT("a"), 1);
	TestTrue(TEXT("still a string"), Str.IsString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonApiArrayTest, "VaRest.Json.Api.Arrays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonApiArrayTest::RunTest(const FString& Parameters)
{
	FVaRestJson A = FVaRestJson::Array(1, 2, 3);
	A.Add(4).SetAt(0, TEXT("zero"));
	TestEqual(TEXT("Num"), A.Num(), 4);
	TestEqual(TEXT("SetAt"), A[0].AsString(), FString(TEXT("zero")));
	TestTrue(TEXT("RemoveAt"), A.RemoveAt(1));
	TestEqual(TEXT("shifted"), A[1].AsInt(), 3);
	TestFalse(TEXT("RemoveAt out of range"), A.RemoveAt(99));
	TestFalse(TEXT("At out of range is invalid"), A[99].IsValid());
	TestFalse(TEXT("negative index is invalid"), A[-1].IsValid());

	int32 Sum = 0;
	for (const FVaRestJson& Item : A.Elements())
	{
		Sum += Item.AsInt();
	}
	TestEqual(TEXT("Elements()"), Sum, 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonApiIterationTest, "VaRest.Json.Api.MembersIteration",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonApiIterationTest::RunTest(const FString& Parameters)
{
	FVaRestJson J = FVaRestJson::Object().Set(TEXT("b"), 1).Set(TEXT("a"), 2).Set(TEXT("c"), 3);
	TArray<FString> Keys;
	for (const auto& [Key, Value] : J.Members())
	{
		Keys.Add(FString::ConstructFromPtrSize(Key.GetData(), Key.Len()));
	}
	TestEqual(TEXT("insertion order"), FString::Join(Keys, TEXT(",")), FString(TEXT("b,a,c")));
	TestEqual(TEXT("GetKeys"), FString::Join(J.GetKeys(), TEXT(",")), FString(TEXT("b,a,c")));

	// Removing while iterating must not crash or read out of range
	int32 Visited = 0;
	for (const FVaRestJsonMember& M : J.Members())
	{
		++Visited;
		J.Remove(M.Key);
	}
	// Deterministic: visit b (removed -> [a,c]), index 1 is c (removed -> [a]), index 2 >= Num stops.
	TestEqual(TEXT("visits"), Visited, 2);
	TestEqual(TEXT("survivor"), FString::Join(J.GetKeys(), TEXT(",")), FString(TEXT("a")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonApiConcurrentReadTest, "VaRest.Json.Api.ConcurrentReaders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonApiConcurrentReadTest::RunTest(const FString& Parameters)
{
	FVaRestJson J = FVaRestJson::Object();
	for (int32 i = 0; i < 2000; ++i)
	{
		J.Set(*FString::Printf(TEXT("key%d"), i), i);
	}
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
	TestEqual(TEXT("concurrent lookups"), Wrong.load(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonApiEdgeCasesTest, "VaRest.Json.Api.EdgeCases",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonApiEdgeCasesTest::RunTest(const FString& Parameters)
{
	// Subtree Clone must not inherit the whole document's arena size
	FVaRestJson Big = FVaRestJson::Object();
	for (int32 i = 0; i < 2000; ++i)
	{
		Big.Set(*FString::Printf(TEXT("key%d"), i), FString::Printf(TEXT("value number %d"), i));
	}
	FVaRestJson Sub = Big[TEXT("key5")].Clone();
	TestTrue(TEXT("subtree clone arena is small"), Sub.GetDocumentInternal()->Arena.GetReservedBytes() < 64 * 1024);
	TestEqual(TEXT("subtree clone value"), Sub.AsString(), FString(TEXT("value number 5")));
	TestTrue(TEXT("root clone equals"), Big.Clone().Equals(Big));

	// UTF-8 Remove
	FVaRestJson R = FVaRestJson::Object().Set(TEXT("a"), 1).Set(TEXT("b"), 2);
	TestTrue(TEXT("Remove utf8"), R.Remove(FUtf8StringView(UTF8TEXT("a"))));
	TestFalse(TEXT("removed"), R.Has(TEXT("a")));
	TestFalse(TEXT("Remove utf8 missing"), R.Remove(UTF8TEXT("zzz")));

	// A missed lookup does not swallow writes
	FVaRestJson Parent = FVaRestJson::Object();
	AddExpectedError(TEXT("cannot Set a field on a missing value"), EAutomationExpectedErrorFlags::Contains, 1);
	Parent[TEXT("profile")].Set(TEXT("x"), 1);
	TestEqual(TEXT("parent unchanged"), Parent.Num(), 0);
	TestFalse(TEXT("miss stays invalid"), Parent[TEXT("profile")].IsValid());
	FVaRestJson Def;
	Def.Set(TEXT("x"), 1);
	TestTrue(TEXT("default handle still auto-vivifies"), Def.IsObject());
	TestTrue(TEXT("node-less handles equal"), Parent[TEXT("a")].Equals(Parent[TEXT("b")]));

	// TryGet<float> range
	FVaRestJson F = FVaRestJson::Object().Set(TEXT("huge"), 1e300).Set(TEXT("ok"), 1.5);
	TestFalse(TEXT("1e300 is not a float"), F[TEXT("huge")].TryGet<float>().IsSet());
	TestEqual(TEXT("1.5 float"), F[TEXT("ok")].AsFloat(), 1.5f);

	// SetAt out of range
	FVaRestJson A = FVaRestJson::Array(1, 2);
	AddExpectedError(TEXT("cannot SetAt index 5"), EAutomationExpectedErrorFlags::Contains, 1);
	A.SetAt(5, 9);
	TestEqual(TEXT("SetAt oob leaves Num"), A.Num(), 2);

	// Replacing a key keeps Num
	FVaRestJson S = FVaRestJson::Object().Set(TEXT("k"), 1).Set(TEXT("k"), 2);
	TestEqual(TEXT("replace keeps Num"), S.Num(), 1);
	TestEqual(TEXT("replaced"), S[TEXT("k")].AsInt(), 2);

	// Small unsigned reads with AsInt
	FVaRestJson U = FVaRestJson::Object().Set(TEXT("u"), 5u);
	TestEqual(TEXT("5u AsInt"), U[TEXT("u")].AsInt(), 5);
	return true;
}

#endif
