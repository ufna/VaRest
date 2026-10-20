// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/ParallelFor.h"
#include "Misc/AutomationTest.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonConvertPrivate.h"
#include "VaRestJsonConvertTestTypes.h"

namespace VaRestJsonConvertNamesTest
{
	FString NameOf(const VaRest::Json::Convert::FFieldInfo& Field)
	{
		return FString::ConstructFromPtrSize(Field.Name.GetData(), Field.Name.Num());
	}

	FString Names(const VaRest::Json::Convert::FFieldMapRef& Map)
	{
		TArray<FString> Result;
		for (const VaRest::Json::Convert::FFieldInfo& Field : Map->Fields)
		{
			Result.Add(NameOf(Field));
		}
		return FString::Join(Result, TEXT(","));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonConvertNamingTest, "VaRest.Json.Convert.Naming",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonConvertNamingTest::RunTest(const FString& Parameters)
{
	using VaRest::Json::Convert::ApplyNaming;
	struct FCase
	{
		const TCHAR* Authored;
		bool bBool;
		const TCHAR* AsIs;
		const TCHAR* Camel;
		const TCHAR* Snake;
		const TCHAR* Pascal;
	};
	const FCase Cases[] = {
		{ TEXT("UserId"),      false, TEXT("UserId"),      TEXT("userId"),      TEXT("user_id"),      TEXT("UserId") },
		{ TEXT("UserID"),      false, TEXT("UserID"),      TEXT("userId"),      TEXT("user_id"),      TEXT("UserId") },
		{ TEXT("URLPath"),     false, TEXT("URLPath"),     TEXT("urlPath"),     TEXT("url_path"),     TEXT("UrlPath") },
		{ TEXT("bIsActive"),   true,  TEXT("bIsActive"),   TEXT("isActive"),    TEXT("is_active"),    TEXT("IsActive") },
		{ TEXT("bIsActive"),   false, TEXT("bIsActive"),   TEXT("bIsActive"),   TEXT("b_is_active"),  TEXT("BIsActive") },
		{ TEXT("HTTPStatus2"), false, TEXT("HTTPStatus2"), TEXT("httpStatus2"), TEXT("http_status2"), TEXT("HttpStatus2") },
		{ TEXT("Vector3D"),    false, TEXT("Vector3D"),    TEXT("vector3D"),    TEXT("vector3_d"),    TEXT("Vector3D") },
		{ TEXT("user_name"),   false, TEXT("user_name"),   TEXT("userName"),    TEXT("user_name"),    TEXT("UserName") },
		{ TEXT("X"),           false, TEXT("X"),           TEXT("x"),           TEXT("x"),            TEXT("X") },
		{ TEXT("ID"),          false, TEXT("ID"),          TEXT("id"),          TEXT("id"),           TEXT("Id") },
		{ TEXT("maxHP"),       false, TEXT("maxHP"),       TEXT("maxHp"),       TEXT("max_hp"),       TEXT("MaxHp") },
	};
	for (const FCase& Case : Cases)
	{
		TestEqualSensitive(FString::Printf(TEXT("%s AsIs"), Case.Authored), FStringView(ApplyNaming(Case.Authored, Case.bBool, EVaRestJsonNaming::AsIs)), FStringView(Case.AsIs));
		TestEqualSensitive(FString::Printf(TEXT("%s camelCase"), Case.Authored), FStringView(ApplyNaming(Case.Authored, Case.bBool, EVaRestJsonNaming::CamelCase)), FStringView(Case.Camel));
		TestEqualSensitive(FString::Printf(TEXT("%s snake_case"), Case.Authored), FStringView(ApplyNaming(Case.Authored, Case.bBool, EVaRestJsonNaming::SnakeCase)), FStringView(Case.Snake));
		TestEqualSensitive(FString::Printf(TEXT("%s PascalCase"), Case.Authored), FStringView(ApplyNaming(Case.Authored, Case.bBool, EVaRestJsonNaming::PascalCase)), FStringView(Case.Pascal));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonConvertUserStructNameTest, "VaRest.Json.Convert.UserStructNameAlgorithm",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonConvertUserStructNameTest::RunTest(const FString& Parameters)
{
	using VaRest::Json::Convert::StripGuidSuffix;
	// The cooked-build algorithm of UUserDefinedStruct::GetAuthoredNameForField (UserDefinedStruct.cpp:300-313)
	TestEqualSensitive(TEXT("Name_<n>_<guid>"), StripGuidSuffix(TEXT("Score_3_0123456789ABCDEF0123456789ABCDEF")), FString(TEXT("Score")));
	TestEqualSensitive(TEXT("underscores inside the name survive"), StripGuidSuffix(TEXT("My_Var_12_0123456789ABCDEF0123456789ABCDEF")), FString(TEXT("My_Var")));
	TestEqualSensitive(TEXT("a short (C++) name is untouched"), StripGuidSuffix(TEXT("Score")), FString(TEXT("Score")));
	TestEqualSensitive(TEXT("35 characters or fewer are untouched"), StripGuidSuffix(TEXT("A_0123456789ABCDEF0123456789ABCDEF")), FString(TEXT("A_0123456789ABCDEF0123456789ABCDEF")));
	TestEqualSensitive(TEXT("no '_' after position 0 keeps the whole name"), StripGuidSuffix(TEXT("_12_0123456789ABCDEF0123456789ABCDEF")), FString(TEXT("_12_0123456789ABCDEF0123456789ABCDEF")));
	TestEqualSensitive(TEXT("C++ struct fields use the property name"),
		VaRest::Json::Convert::AuthoredFieldName(FVaRestJsonTestNaming::StaticStruct(), FindFProperty<FProperty>(FVaRestJsonTestNaming::StaticStruct(), TEXT("URLPath"))),
		FString(TEXT("URLPath")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonConvertFieldMapTest, "VaRest.Json.Convert.FieldMap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonConvertFieldMapTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json;
	using namespace VaRest::Json::Convert;
	using namespace VaRestJsonConvertNamesTest;
	FVaRestJsonConvert::ResetCache();

	const FFieldMapRef Camel = GetFieldMap(FVaRestJsonTestNaming::StaticStruct(), EVaRestJsonNaming::CamelCase);
	TestEqualSensitive(TEXT("camelCase names, transient skipped"), Names(Camel), FString(TEXT("userId,isActive,urlPath,httpStatus2,userName")));
	TestEqualSensitive(TEXT("snake_case names"), Names(GetFieldMap(FVaRestJsonTestNaming::StaticStruct(), EVaRestJsonNaming::SnakeCase)), FString(TEXT("user_id,is_active,url_path,http_status2,user_name")));
	TestEqualSensitive(TEXT("AsIs names"), Names(GetFieldMap(FVaRestJsonTestNaming::StaticStruct(), EVaRestJsonNaming::AsIs)), FString(TEXT("UserId,bIsActive,URLPath,HTTPStatus2,user_name")));
	TestTrue(TEXT("a second lookup returns the cached map"), &GetFieldMap(FVaRestJsonTestNaming::StaticStruct(), EVaRestJsonNaming::CamelCase).Get() == &Camel.Get());
	TestEqual(TEXT("one map per (struct, policy)"), FVaRestJsonConvert::NumCachedFieldMapsInternal(), 3);

	// Review Focus: two fields with one JSON name -- the later one is skipped, so no key is written twice
	AddExpectedMessagePlain(TEXT("is already used by another field"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const FFieldMapRef Collision = GetFieldMap(FVaRestJsonTestCollision::StaticStruct(), EVaRestJsonNaming::CamelCase);
	TestEqual(TEXT("one field survives"), Collision->Fields.Num(), 1);
	TestTrue(TEXT("the first one"), Collision->Fields.Num() == 1 && Collision->Fields[0].Property->GetFName() == FName(TEXT("user_id")));

	// Unsupported fields: one warning each per map build (the struct's reflected name has no 'F')
	AddExpectedMessagePlain(TEXT("VaRestJsonTestSkipped."), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 3);
	const FFieldMapRef Skipped = GetFieldMap(FVaRestJsonTestSkipped::StaticStruct(), EVaRestJsonNaming::CamelCase);
	TestEqualSensitive(TEXT("only Kept is converted"), Names(Skipped), FString(TEXT("kept")));

	// Decode matching tiers
	FUtf8Scratch Scratch;
	uint8 Tier = MAX_uint8;
	const auto MatchKey = [&Camel, &Scratch, &Tier](const UTF8CHAR* Key)
	{
		const FUtf8StringView KeyView(Key);
		return Camel->Match(KeyView, HashKey(KeyView), Scratch, Tier);
	};
	TestEqual(TEXT("exact"), MatchKey(UTF8TEXT("userId")), 0);
	TestEqual(TEXT("exact is tier 0"), static_cast<int32>(Tier), 0);
	TestEqual(TEXT("case-insensitive"), MatchKey(UTF8TEXT("USERID")), 0);
	TestEqual(TEXT("case-insensitive is tier 1"), static_cast<int32>(Tier), 1);
	TestEqual(TEXT("normalised"), MatchKey(UTF8TEXT("user_id")), 0);
	TestEqual(TEXT("normalised is tier 2"), static_cast<int32>(Tier), 2);
	TestEqual(TEXT("a bool without its b"), MatchKey(UTF8TEXT("is_active")), 1);
	Tier = MAX_uint8; // the previous match was tier 2 too
	TestEqual(TEXT("a bool's AsIs spelling, which FJsonObjectConverter writes"), MatchKey(UTF8TEXT("bIsActive")), 1);
	TestEqual(TEXT("an AsIs bool is tier 2"), static_cast<int32>(Tier), 2);
	TestEqual(TEXT("only a bool drops a b"), MatchKey(UTF8TEXT("bUserId")), INDEX_NONE);
	const FFieldMapRef Bools = GetFieldMap(FVaRestJsonTestBoolPrefix::StaticStruct(), EVaRestJsonNaming::CamelCase);
	const auto MatchBool = [&Bools, &Scratch, &Tier](const UTF8CHAR* Key)
	{
		const FUtf8StringView KeyView(Key);
		return Bools->Match(KeyView, HashKey(KeyView), Scratch, Tier);
	};
	TestEqualSensitive(TEXT("bool names"), Names(Bools), FString(TEXT("on,old")));
	TestEqual(TEXT("bOn reaches the field the policy calls on"), MatchBool(UTF8TEXT("bOn")), 0);
	TestEqual(TEXT("a bool authored without a b does not answer to one"), MatchBool(UTF8TEXT("bold")), INDEX_NONE);
	TestEqual(TEXT("unknown key"), MatchKey(UTF8TEXT("nope")), INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonConvertCacheTest, "VaRest.Json.Convert.FieldMapCacheConcurrent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonConvertCacheTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::Convert;
	FVaRestJsonConvert::ResetCache();
	TestEqual(TEXT("reset empties the cache"), FVaRestJsonConvert::NumCachedFieldMapsInternal(), 0);

	// Decode runs off the game thread: many threads asking for the same maps at once must end with one map per key
	const UScriptStruct* Structs[] = { FVaRestJsonTestNaming::StaticStruct(), FVaRestJsonTestInner::StaticStruct() };
	const EVaRestJsonNaming Policies[] = { EVaRestJsonNaming::AsIs, EVaRestJsonNaming::CamelCase, EVaRestJsonNaming::SnakeCase, EVaRestJsonNaming::PascalCase };
	TArray<const FFieldMap*> Seen;
	Seen.SetNumZeroed(64 * 8);
	ParallelFor(64, [&Seen, &Structs, &Policies](int32 Worker)
	{
		for (int32 k = 0; k < 8; ++k)
		{
			Seen[Worker * 8 + k] = &GetFieldMap(Structs[k % 2], Policies[k / 2]).Get();
		}
	});
	TestEqual(TEXT("eight maps"), FVaRestJsonConvert::NumCachedFieldMapsInternal(), 8);
	int32 Strays = 0;
	for (int32 i = 0; i < Seen.Num(); ++i)
	{
		const int32 k = i % 8;
		Strays += Seen[i] != &GetFieldMap(Structs[k % 2], Policies[k / 2]).Get();
	}
	TestEqual(TEXT("every thread got the cached map"), Strays, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonConvertRenamesTest, "VaRest.Json.Convert.Renames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonConvertRenamesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::Convert;
	using namespace VaRestJsonConvertNamesTest;
	FVaRestJsonNames::Register<FVaRestJsonTestInner>({ { TEXT("Id"), TEXT("user_id") } });
	const FFieldMapRef Renamed = GetFieldMap(FVaRestJsonTestInner::StaticStruct(), EVaRestJsonNaming::CamelCase);
	TestEqualSensitive(TEXT("the rename wins over the policy"), Names(Renamed), FString(TEXT("user_id,name")));
	FVaRestJsonNames::Unregister(FVaRestJsonTestInner::StaticStruct());
	TestEqualSensitive(TEXT("unregistered: the policy again"), Names(GetFieldMap(FVaRestJsonTestInner::StaticStruct(), EVaRestJsonNaming::CamelCase)), FString(TEXT("id,name")));
	return true;
}

#endif
