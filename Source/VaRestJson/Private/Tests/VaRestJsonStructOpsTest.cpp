// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Tests/VaRestJsonTestTypes.h"
#include "VaRestJson.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonStructOpsTextTest, "VaRest.Json.StructOps.Text",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonStructOpsTextTest::RunTest(const FString& Parameters)
{
	UScriptStruct* HolderStruct = FVaRestJsonTestHolder::StaticStruct();

	FVaRestJsonTestHolder Source;
	Source.Before = 3;
	Source.Json = FVaRestJson::Object().Set(TEXT("tricky"), TEXT("a),b,\"c\"(")).Set(TEXT("n"), FVaRestJson::Array(1, 2));
	Source.After = 7;

	FString Text;
	HolderStruct->ExportText(Text, &Source, nullptr, nullptr, PPF_None, nullptr);

	FVaRestJsonTestHolder Imported;
	HolderStruct->ImportText(*Text, &Imported, nullptr, PPF_None, GLog, HolderStruct->GetName());
	TestEqual(TEXT("field before"), Imported.Before, 3);
	TestTrue(TEXT("json survived delimiters inside strings"), Imported.Json.Equals(Source.Json));
	TestEqual(TEXT("field after was not swallowed"), Imported.After, 7);

	// An invalid handle exports as nothing and imports back as invalid
	FVaRestJsonTestHolder Empty;
	Empty.After = 5;
	FString EmptyText;
	HolderStruct->ExportText(EmptyText, &Empty, nullptr, nullptr, PPF_None, nullptr);
	TestTrue(TEXT("empty field is still exported"), EmptyText.Contains(TEXT("Json=")));
	FVaRestJsonTestHolder EmptyBack;
	EmptyBack.Json = FVaRestJson::Object();
	HolderStruct->ImportText(*EmptyText, &EmptyBack, nullptr, PPF_None, GLog, HolderStruct->GetName());
	// UScriptStruct::ImportText never calls ImportTextItem for an empty "Json=," value (engine skips the
	// field), so the pre-set object is left alone there; the empty-value branch is tested directly below.
	TestEqual(TEXT("after the empty field"), EmptyBack.After, 5);

	FVaRestJson EmptyPin = FVaRestJson::Object();
	const TCHAR* EmptyBuffer = TEXT(" ,After=5)");
	TestTrue(TEXT("empty ImportTextItem"), EmptyPin.ImportTextItem(EmptyBuffer, PPF_None, nullptr, GLog));
	TestFalse(TEXT("invalid round trip"), EmptyPin.IsValid());
	TestEqualSensitive(TEXT("buffer stops at the delimiter"), FString(EmptyBuffer), FString(TEXT(",After=5)")));

	// A pin default typed by hand
	FVaRestJson Pin;
	const TCHAR* Buffer = TEXT("  [1, \"x\"] trailing");
	TestTrue(TEXT("ImportTextItem"), Pin.ImportTextItem(Buffer, PPF_None, nullptr, GLog));
	TestEqualSensitive(TEXT("value"), Pin.ToString(), FString(TEXT("[1,\"x\"]")));
	TestEqualSensitive(TEXT("buffer advanced past the value only"), FString(Buffer), FString(TEXT(" trailing")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonStructOpsIdentitySerializeTest, "VaRest.Json.StructOps.IdenticalAndSerialize",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonStructOpsIdentitySerializeTest::RunTest(const FString& Parameters)
{
	UScriptStruct* JsonStruct = FVaRestJson::StaticStruct();
	TestTrue(TEXT("has serializer"), JsonStruct->GetCppStructOps()->HasSerializer());

	const FVaRestJson A = FVaRestJson::Object().Set(TEXT("x"), 1);
	const FVaRestJson B = FVaRestJson::Object().Set(TEXT("x"), 1.0);
	TestTrue(TEXT("Identical is deep"), JsonStruct->CompareScriptStruct(&A, &B, PPF_None));
	const FVaRestJson C = FVaRestJson::Object().Set(TEXT("x"), 2);
	TestFalse(TEXT("different"), JsonStruct->CompareScriptStruct(&A, &C, PPF_None));

	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);
	FVaRestJson ToSave = FVaRestJson::Object().Set(TEXT("s"), TEXT("\u0436")).Set(TEXT("u"), MAX_uint64);
	FVaRestJson Invalid;
	ToSave.Serialize(Writer);
	Invalid.Serialize(Writer);

	FMemoryReader Reader(Bytes);
	FVaRestJson Loaded;
	FVaRestJson LoadedInvalid = FVaRestJson::Object();
	Loaded.Serialize(Reader);
	LoadedInvalid.Serialize(Reader);
	TestTrue(TEXT("serialize round trip"), Loaded.Equals(ToSave));
	TestFalse(TEXT("invalid stays invalid"), LoadedInvalid.IsValid());
	TestFalse(TEXT("reader consumed exactly"), Reader.IsError());
	return true;
}

#endif
