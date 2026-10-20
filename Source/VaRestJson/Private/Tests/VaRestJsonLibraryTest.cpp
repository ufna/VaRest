// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "VaRestJsonLibrary.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonLibraryValuesTest, "VaRest.Json.Library.Values",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonLibraryValuesTest::RunTest(const FString& Parameters)
{
	using L = UVaRestJsonLibrary;
	FVaRestJson Json;
	FVaRestJsonError Error;
	TestTrue(TEXT("Parse Json"), L::ParseJson(TEXT("{\"name\":\"Neo\",\"level\":42,\"big\":9007199254740993,\"ratio\":0.5,\"alive\":true,\"o\":{\"x\":1},\"a\":[1,2,3]}"), Json, Error));
	TestEqualSensitive(TEXT("Get String Field"), L::GetStringField(Json, TEXT("name"), TEXT("?")), FString(TEXT("Neo")));
	TestEqualSensitive(TEXT("... default on a miss"), L::GetStringField(Json, TEXT("nope"), TEXT("?")), FString(TEXT("?")));
	TestEqual(TEXT("Get Int Field"), L::GetIntField(Json, TEXT("level"), -1), 42);
	TestEqual(TEXT("... default on a type mismatch"), L::GetIntField(Json, TEXT("name"), -1), -1);
	TestEqual(TEXT("Get Int64 Field keeps 64 bits"), L::GetInt64Field(Json, TEXT("big"), 0), 9007199254740993ll);
	TestTrue(TEXT("Get Number Field"), L::GetNumberField(Json, TEXT("ratio"), 0.0) == 0.5);
	TestTrue(TEXT("Get Bool Field"), L::GetBoolField(Json, TEXT("alive"), false));
	TestTrue(TEXT("Get Object Field"), L::GetObjectField(Json, TEXT("o"), FVaRestJson()).IsObject());
	TestTrue(TEXT("Get Array Field"), L::GetArrayField(Json, TEXT("a"), FVaRestJson()).IsArray());
	TestFalse(TEXT("Get Object Field default on a mismatch"), L::GetObjectField(Json, TEXT("a"), FVaRestJson()).IsValid());

	// Reference semantics: the object field is the same JSON, so a write through it shows in the parent
	FVaRestJson Inner = L::GetObjectField(Json, TEXT("o"), FVaRestJson());
	Inner.Set(TEXT("y"), 2);
	TestEqual(TEXT("handles alias the document"), Json.Path(TEXT("o.y")).AsInt(), 2);

	TestFalse(TEXT("Parse Json failure"), L::ParseJson(TEXT("{\"a\":"), Json, Error));
	TestTrue(TEXT("... invalid Json and a located error"), !Json.IsValid() && Error.Line == 1 && !Error.Message.IsEmpty());

	TestEqualSensitive(TEXT("From Number writes integers like JSON.stringify"), L::JsonToString(L::JsonFromNumber(3.0)), FString(TEXT("3")));
	TestEqualSensitive(TEXT("From Number keeps fractions"), L::JsonToString(L::JsonFromNumber(2.5)), FString(TEXT("2.5")));
	TestEqualSensitive(TEXT("From Bool / String / Null"), L::JsonToString(L::JsonFromBool(true)) + L::JsonToString(L::JsonFromString(TEXT("s"))) + L::JsonToString(L::JsonNull()), FString(TEXT("true\"s\"null")));
	TestTrue(TEXT("Get Json Type"), L::GetJsonType(L::MakeJsonArray()) == EVaRestJsonType::Array);
	TestTrue(TEXT("Is Valid / Is Null"), !L::IsValidJson(FVaRestJson()) && L::IsNullJson(L::JsonNull()));
	// A getter on a handle that is not an object is a miss, never a crash
	TestEqual(TEXT("getters on an invalid handle give the default"), L::GetIntField(FVaRestJson(), TEXT("a"), 7), 7);
	TestFalse(TEXT("... and Has Field is false"), L::HasField(FVaRestJson::Array(1), TEXT("a")));
	TestEqualSensitive(TEXT("To String pretty"), L::JsonToString(FVaRestJson::Object().Set(TEXT("a"), 1), true), FString(TEXT("{\n  \"a\": 1\n}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonLibraryEditTest, "VaRest.Json.Library.ObjectsAndArrays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonLibraryEditTest::RunTest(const FString& Parameters)
{
	using L = UVaRestJsonLibrary;
	const FVaRestJson Target = FVaRestJson::Object().Set(TEXT("a"), 1).Set(TEXT("b"), 2);
	TestEqualSensitive(TEXT("Get Keys keeps order"), FString::Join(L::GetJsonKeys(Target), TEXT(",")), FString(TEXT("a,b")));
	TestTrue(TEXT("Has Field"), L::HasField(Target, TEXT("a")) && !L::HasField(Target, TEXT("A")));
	TestTrue(TEXT("Remove Field"), L::RemoveField(Target, TEXT("a")) && !L::HasField(Target, TEXT("a")));
	const FVaRestJson Merged = L::MergeJson(Target, FVaRestJson::Object().Set(TEXT("b"), 3).Set(TEXT("c"), 4));
	TestEqualSensitive(TEXT("Merge: shallow, later wins"), L::JsonToString(Merged), FString(TEXT("{\"b\":3,\"c\":4}")));
	TestTrue(TEXT("Merge writes into Target (handles alias)"), Merged.IsSameNode(Target));

	const FVaRestJson List = FVaRestJson::Array(1, 2, 3);
	TestEqual(TEXT("Length"), L::JsonLength(List), 3);
	TestTrue(TEXT("Array Remove At"), L::ArrayRemoveAt(List, 0) && L::JsonLength(List) == 2);
	const FVaRestJson Copy = L::CloneJson(List);
	TestTrue(TEXT("Clone is equal and independent"), L::JsonEquals(Copy, List) && !Copy.IsSameNode(List));
	TestFalse(TEXT("Equals is deep"), L::JsonEquals(Copy, FVaRestJson::Array(2, 4)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonLibraryFilesTest, "VaRest.Json.Library.Files",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonLibraryFilesTest::RunTest(const FString& Parameters)
{
	using L = UVaRestJsonLibrary;
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("VaRestTests");
	const FString Path = Dir / TEXT("library.json");
	const FVaRestJson Original = FVaRestJson::Object().Set(TEXT("name"), TEXT("\u041a\u043b\u044e\u0447")).Set(TEXT("n"), 1);
	TestTrue(TEXT("Save Json File"), L::SaveJsonFile(Original, Path));
	TArray<uint8> Raw;
	FFileHelper::LoadFileToArray(Raw, *Path);
	TestTrue(TEXT("UTF-8 without a BOM"), Raw.Num() > 3 && Raw[0] == '{');
	FVaRestJson Loaded;
	FVaRestJsonError Error;
	TestTrue(TEXT("Load Json File"), L::LoadJsonFile(Path, Loaded, Error) && Loaded.Equals(Original));

	// A file saved by an editor with a BOM still loads
	TArray<uint8> WithBom = { 0xEF, 0xBB, 0xBF };
	WithBom.Append(Raw);
	FFileHelper::SaveArrayToFile(WithBom, *Path);
	TestTrue(TEXT("BOM is skipped"), L::LoadJsonFile(Path, Loaded, Error) && Loaded.Equals(Original));

	TestFalse(TEXT("a missing file fails"), L::LoadJsonFile(Dir / TEXT("missing.json"), Loaded, Error));
	TestTrue(TEXT("... with a message and an invalid Json"), !Loaded.IsValid() && Error.Message.Contains(TEXT("missing.json")));
	IFileManager::Get().Delete(*Path);
	return true;
}

#endif
