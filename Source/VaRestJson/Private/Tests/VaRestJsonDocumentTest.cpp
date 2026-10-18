// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJsonDocument.h"

using namespace VaRest::Json;

namespace VaRestJsonDocumentTest
{
	FUtf8StringView K(const char* S) { return FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(S)); }

	/** A missing member fails the test and yields an inert Null node instead of a null dereference. */
	const FNode& Need(FAutomationTestBase& Test, const FNode* Node, const TCHAR* What)
	{
		static FNode Missing;
		if (!Node)
		{
			Test.AddError(FString::Printf(TEXT("Missing node: %s"), What));
			return Missing;
		}
		return *Node;
	}
}
using VaRestJsonDocumentTest::K;
using VaRestJsonDocumentTest::Need;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDocNumbersTest, "VaRest.Json.Document.Numbers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDocNumbersTest::RunTest(const FString& Parameters)
{
	FDocument Doc;
	FNode* Small = Doc.NewUInt(42);
	TestTrue(TEXT("uint that fits int64 is stored as Int"), Small->NumKind == ENumKind::Int && Small->Int == 42);
	FNode* Big = Doc.NewUInt(MAX_uint64);
	TestTrue(TEXT("uint above INT64_MAX is stored as UInt"), Big->NumKind == ENumKind::UInt && Big->UInt == MAX_uint64);
	TestTrue(TEXT("1 == 1.0"), FDocument::Equals(*Doc.NewInt(1), *Doc.NewDouble(1.0)));
	TestFalse(TEXT("1 != 1.5"), FDocument::Equals(*Doc.NewInt(1), *Doc.NewDouble(1.5)));
	TestTrue(TEXT("2^63 as UInt == 2^63 as double"), FDocument::Equals(*Doc.NewUInt(9223372036854775808ull), *Doc.NewDouble(9223372036854775808.0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDocObjectTest, "VaRest.Json.Document.ObjectMembers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDocObjectTest::RunTest(const FString& Parameters)
{
	FDocument Doc;
	FNode* Obj = Doc.NewObject();
	Doc.Set(*Obj, K("a"), Doc.NewInt(1));
	Doc.Set(*Obj, K("b"), Doc.NewInt(2));
	Doc.Set(*Obj, K("a"), Doc.NewInt(3)); // replaces in place
	TestEqual(TEXT("replace keeps member count"), (int32)Obj->Obj.Num, 2);
	TestTrue(TEXT("replace keeps order"), KeyEquals(Obj->Obj.Members[0].Key, K("a")));
	TestEqual(TEXT("a replaced"), Need(*this, FDocument::Find(*Obj, K("a")), TEXT("a")).Int, (int64)3);

	// Keys are case-sensitive
	TestNull(TEXT("'A' is not 'a'"), FDocument::Find(*Obj, K("A")));

	// Duplicates (the parser keeps them): lookup returns the LAST occurrence
	Doc.Append(*Obj, Doc.CopyStr(K("b"), true), Doc.NewInt(20));
	TestEqual(TEXT("duplicate kept"), (int32)Obj->Obj.Num, 3);
	TestEqual(TEXT("last occurrence wins"), Need(*this, FDocument::Find(*Obj, K("b")), TEXT("b")).Int, (int64)20);

	// Remove drops every occurrence and keeps the order of the rest
	Doc.Set(*Obj, K("c"), Doc.NewInt(4));
	TestEqual(TEXT("removed both b"), Doc.Remove(*Obj, K("b")), 2);
	TestEqual(TEXT("two left"), (int32)Obj->Obj.Num, 2);
	TestTrue(TEXT("order a, c"), KeyEquals(Obj->Obj.Members[0].Key, K("a")) && KeyEquals(Obj->Obj.Members[1].Key, K("c")));
	TestEqual(TEXT("removing a missing key"), Doc.Remove(*Obj, K("zzz")), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDocIndexTest, "VaRest.Json.Document.Index",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDocIndexTest::RunTest(const FString& Parameters)
{
	FDocument Doc;
	FNode* Obj = Doc.NewObject();
	for (int32 i = 0; i < (int32)IndexThreshold; ++i)
	{
		Doc.Set(*Obj, StringCast<UTF8CHAR>(*FString::Printf(TEXT("k%d"), i)).Get(), Doc.NewInt(i));
	}
	TestNull(TEXT("no index at the threshold"), Obj->Obj.Index);
	for (int32 i = (int32)IndexThreshold; i < 1000; ++i)
	{
		Doc.Set(*Obj, StringCast<UTF8CHAR>(*FString::Printf(TEXT("k%d"), i)).Get(), Doc.NewInt(i));
	}
	TestNotNull(TEXT("index above the threshold"), Obj->Obj.Index);

	int32 Wrong = 0;
	for (int32 i = 0; i < 1000; ++i)
	{
		const FNode* N = FDocument::Find(*Obj, StringCast<UTF8CHAR>(*FString::Printf(TEXT("k%d"), i)).Get());
		Wrong += !N || N->Int != i;
	}
	TestEqual(TEXT("indexed lookups"), Wrong, 0);

	// Duplicate through the index: last wins
	Doc.Append(*Obj, Doc.CopyStr(K("k5"), true), Doc.NewInt(5000));
	TestEqual(TEXT("indexed last occurrence wins"), Need(*this, FDocument::Find(*Obj, K("k5")), TEXT("k5")).Int, (int64)5000);

	// Remove shifts members; the index must be rebuilt, not left pointing at old slots
	TestEqual(TEXT("remove k0"), Doc.Remove(*Obj, K("k0")), 1);
	TestEqual(TEXT("k999 after shift"), Need(*this, FDocument::Find(*Obj, K("k999")), TEXT("k999")).Int, (int64)999);
	TestNull(TEXT("k0 gone"), FDocument::Find(*Obj, K("k0")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDocStabilityTest, "VaRest.Json.Document.ChildSurvivesParentGrowth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDocStabilityTest::RunTest(const FString& Parameters)
{
	FDocument Doc;
	FNode* Root = Doc.NewObject();
	FNode* Child = Doc.NewObject();
	Doc.Set(*Root, K("child"), Child);
	Doc.Set(*Child, K("x"), Doc.NewString(K("kept")));
	FNode* Arr = Doc.NewArray();
	Doc.Set(*Root, K("arr"), Arr);
	FNode* First = Doc.NewInt(7);
	Doc.Append(*Arr, First);

	for (int32 i = 0; i < 1000; ++i)
	{
		Doc.Set(*Root, StringCast<UTF8CHAR>(*FString::Printf(TEXT("grow%d"), i)).Get(), Doc.NewInt(i));
		Doc.Append(*Arr, Doc.NewInt(i));
	}

	TestTrue(TEXT("child pointer still resolves"), FDocument::Find(*Root, K("child")) == Child);
	TestTrue(TEXT("child content intact"), KeyEquals(Need(*this, FDocument::Find(*Child, K("x")), TEXT("x")).Str, K("kept")));
	TestTrue(TEXT("first element pointer intact"), Arr->Arr.Num > 0 && Arr->Arr.Items[0] == First && First->Int == 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDocCopyTest, "VaRest.Json.Document.DeepCopy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDocCopyTest::RunTest(const FString& Parameters)
{
	FDocument Target;
	FNode* Copy = nullptr;
	{
		FDocument Source;
		FNode* Obj = Source.NewObject();
		Source.Set(*Obj, K("s"), Source.NewString(K("text")));
		FNode* Arr = Source.NewArray();
		Source.Append(*Arr, Source.NewDouble(2.5));
		Source.Set(*Obj, K("a"), Arr);
		for (int32 i = 0; i < 20; ++i)
		{
			Source.Set(*Obj, StringCast<UTF8CHAR>(*FString::Printf(TEXT("n%d"), i)).Get(), Source.NewInt(i));
		}
		Copy = Target.DeepCopy(*Obj);
	} // Source and its arena are gone

	TestTrue(TEXT("string copied"), KeyEquals(Need(*this, FDocument::Find(*Copy, K("s")), TEXT("s")).Str, K("text")));
	const FNode& CopiedArr = Need(*this, FDocument::Find(*Copy, K("a")), TEXT("a"));
	const bool bArrOk = CopiedArr.Type == EVaRestJsonType::Array && CopiedArr.Arr.Num == 1;
	TestTrue(TEXT("array copied has one element"), bArrOk);
	TestEqual(TEXT("array copied"), bArrOk ? Need(*this, CopiedArr.Arr.Items[0], TEXT("a[0]")).Double : 0.0, 2.5);
	TestNotNull(TEXT("copied big object is indexed"), Copy->Obj.Index);
	TestEqual(TEXT("indexed lookup in copy"), Need(*this, FDocument::Find(*Copy, K("n19")), TEXT("n19")).Int, (int64)19);

	// Copying an ancestor under one of its own descendants terminates and does not alias
	FNode* Root = Target.NewObject();
	FNode* Sub = Target.NewObject();
	Target.Set(*Root, K("sub"), Sub);
	Target.Set(*Sub, K("loop"), Target.DeepCopy(*Root));
	const FNode* Loop = FDocument::Find(*Sub, K("loop"));
	TestTrue(TEXT("loop is a copy, not the root"), Loop != Root);
	const FNode* LoopSub = Loop && Loop->Type == EVaRestJsonType::Object ? FDocument::Find(*Loop, K("sub")) : nullptr;
	TestNotNull(TEXT("copy contains sub"), LoopSub);
	if (LoopSub && LoopSub->Type == EVaRestJsonType::Object)
	{
		TestNull(TEXT("copy was taken before the attach"), FDocument::Find(*LoopSub, K("loop")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDocEqualsTest, "VaRest.Json.Document.Equals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDocEqualsTest::RunTest(const FString& Parameters)
{
	FDocument Doc;
	FNode* A = Doc.NewObject();
	Doc.Set(*A, K("x"), Doc.NewInt(1));
	Doc.Set(*A, K("y"), Doc.NewString(K("s")));
	FNode* B = Doc.NewObject();
	Doc.Set(*B, K("y"), Doc.NewString(K("s")));
	Doc.Set(*B, K("x"), Doc.NewDouble(1.0));
	TestTrue(TEXT("objects compare without regard to order"), FDocument::Equals(*A, *B));

	Doc.Append(*B, Doc.CopyStr(K("x"), true), Doc.NewInt(2));
	TestFalse(TEXT("duplicates compare by their last occurrence"), FDocument::Equals(*A, *B));

	FNode* L1 = Doc.NewArray();
	Doc.Append(*L1, Doc.NewInt(1));
	Doc.Append(*L1, Doc.NewInt(2));
	FNode* L2 = Doc.NewArray();
	Doc.Append(*L2, Doc.NewInt(2));
	Doc.Append(*L2, Doc.NewInt(1));
	TestFalse(TEXT("arrays are ordered"), FDocument::Equals(*L1, *L2));
	TestFalse(TEXT("strings are case-sensitive"), FDocument::Equals(*Doc.NewString(K("a")), *Doc.NewString(K("A"))));
	return true;
}

#endif
