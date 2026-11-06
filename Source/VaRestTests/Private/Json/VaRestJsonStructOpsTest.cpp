// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "VaRestJsonTestTypes.h"
#include "VaRestJson.h"
#include "VaRestJsonDocument.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonStructOpsImportRetentionTest, "VaRest.Json.StructOps.ImportKeepsOnlyTheValue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonStructOpsImportRetentionTest::RunTest(const FString& Parameters)
{
	// A handle imported from the middle of a long text (a T3D blob, a struct's exported text) must keep
	// only its own value's bytes, not the whole remaining text.
	FString Text = TEXT("[1]");
	Text += TEXT(",After=7)");
	Text += FString::ChrN(64 * 1024, TEXT('x'));
	const TCHAR* Buffer = *Text;

	FVaRestJson Pin;
	TestTrue(TEXT("ImportTextItem"), Pin.ImportTextItem(Buffer, PPF_None, nullptr, GLog));
	TestEqualSensitive(TEXT("value"), Pin.ToString(), FString(TEXT("[1]")));
	TestEqual(TEXT("consumed exactly the value"), static_cast<int32>(Buffer - *Text), 3);
	if (Pin.GetDocumentInternal())
	{
		const TArray<uint8>& Input = Pin.GetDocumentInternal()->Input;
		TestTrue(*FString::Printf(TEXT("document input is the value + NUL (Num %d)"), Input.Num()), Input.Num() <= 3 + 1);
		TestTrue(*FString::Printf(TEXT("document input allocation is small (%llu B)"), (uint64)Input.GetAllocatedSize()), Input.GetAllocatedSize() < 256);
	}
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
	TestTrue(TEXT("reader at end"), Reader.AtEnd());
	TestEqual(TEXT("Tell == Num"), Reader.Tell(), static_cast<int64>(Bytes.Num()));
	return true;
}

namespace VaRestJsonStructOpsTest // named, not anonymous: unity blobs share one anonymous namespace
{
	TArray<uint8> GoldenBytes()
	{
		TArray<uint8> Bytes;
		FMemoryWriter Writer(Bytes);
		FVaRestJson::Object().Set(TEXT("a"), 1).Serialize(Writer);
		return Bytes;
	}

	/** Counts the bytes a struct writes into it; optionally a reference collector, optionally saving */
	class FCountingArchive : public FArchive
	{
	public:
		FCountingArchive(bool bReferenceCollector, bool bSaving)
		{
			ArIsObjectReferenceCollector = bReferenceCollector;
			SetIsSaving(bSaving);
		}

		virtual void Serialize(void* Data, int64 Length) override
		{
			Written += Length;
		}

		int64 Written = 0;
	};

	FVaRestJson LoadFrom(const TArray<uint8>& Bytes, bool& bOutError)
	{
		FMemoryReader Reader(Bytes);
		FVaRestJson Loaded = FVaRestJson::Object();
		Loaded.Serialize(Reader);
		bOutError = Reader.IsError();
		return Loaded;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonStructOpsHostileArchiveTest, "VaRest.Json.StructOps.HostileArchive",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonStructOpsHostileArchiveTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Golden = VaRestJsonStructOpsTest::GoldenBytes();
	const uint8 Expected[] = {1, 1, 7, 0, 0, 0, 0, 0, 0, 0, '{', '"', 'a', '"', ':', '1', '}'};
	TestEqual(TEXT("golden size"), Golden.Num(), static_cast<int32>(sizeof(Expected)));
	TestTrue(TEXT("golden bytes"), Golden.Num() == sizeof(Expected) && FMemory::Memcmp(Golden.GetData(), Expected, sizeof(Expected)) == 0);

	bool bError = false;
	FVaRestJson Good = VaRestJsonStructOpsTest::LoadFrom(Golden, bError);
	TestTrue(TEXT("golden loads"), Good.IsValid() && !bError);

	// (b) truncated mid-payload
	{
		TArray<uint8> Cut(Golden.GetData(), 13);
		AddExpectedError(TEXT("implausible payload length"), EAutomationExpectedErrorFlags::Contains, 0);
		const FVaRestJson Loaded = VaRestJsonStructOpsTest::LoadFrom(Cut, bError);
		TestFalse(TEXT("truncated invalid"), Loaded.IsValid());
		TestTrue(TEXT("truncated errors"), bError);
	}
	// (c) oversized length, patched to 2^40
	{
		TArray<uint8> Huge = Golden;
		FMemory::Memzero(Huge.GetData() + 2, 8);
		Huge[2 + 5] = 1;
		AddExpectedError(TEXT("implausible payload length"), EAutomationExpectedErrorFlags::Contains, 1);
		const FVaRestJson Loaded = VaRestJsonStructOpsTest::LoadFrom(Huge, bError);
		TestFalse(TEXT("huge invalid"), Loaded.IsValid());
		TestTrue(TEXT("huge errors"), bError);
	}
	// (d) wrong version
	{
		TArray<uint8> Bad = Golden;
		Bad[0] = 2;
		AddExpectedError(TEXT("unsupported serialized version 2"), EAutomationExpectedErrorFlags::Contains, 1);
		const FVaRestJson Loaded = VaRestJsonStructOpsTest::LoadFrom(Bad, bError);
		TestFalse(TEXT("version invalid"), Loaded.IsValid());
		TestTrue(TEXT("version errors"), bError);
	}
	// (e) consistent archive, payload is not JSON
	{
		TArray<uint8> Bad = Golden;
		Bad.SetNum(10);
		Bad[2] = 4;
		Bad.Append(reinterpret_cast<const uint8*>("{bad"), 4);
		AddExpectedError(TEXT("stored JSON does not parse"), EAutomationExpectedErrorFlags::Contains, 1);
		const FVaRestJson Loaded = VaRestJsonStructOpsTest::LoadFrom(Bad, bError);
		TestFalse(TEXT("bad json invalid"), Loaded.IsValid());
		TestFalse(TEXT("bad json is not an archive error"), bError);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonStructOpsReferenceCollectorTest, "VaRest.Json.StructOps.ReferenceCollectorSkipsText",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonStructOpsReferenceCollectorTest::RunTest(const FString& Parameters)
{
	FVaRestJson J = FVaRestJson::Object().Set(TEXT("a"), FString::ChrN(1000, TEXT('x')));

	VaRestJsonStructOpsTest::FCountingArchive Collector(true, false);
	TestTrue(TEXT("collector: Serialize handled"), J.Serialize(Collector));
	TestEqual(TEXT("a pure reference collector gets no JSON text"), Collector.Written, (int64)0);
	TestTrue(TEXT("handle untouched"), J[TEXT("a")].AsString().Len() == 1000);

	VaRestJsonStructOpsTest::FCountingArchive SavingCollector(true, true);
	J.Serialize(SavingCollector);
	TestTrue(TEXT("a collector that saves still gets the full format"), SavingCollector.Written > 1000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonStructOpsUnicodeTextTest, "VaRest.Json.StructOps.UnicodeText",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonStructOpsUnicodeTextTest::RunTest(const FString& Parameters)
{
	UScriptStruct* HolderStruct = FVaRestJsonTestHolder::StaticStruct();
	FVaRestJsonTestHolder Source;
	Source.Before = 1;
	Source.Json = FVaRestJson::Object().Set(TEXT("s"), TEXT("\u0436\U0001F600\u0436"));
	Source.After = 7;

	FString Text;
	HolderStruct->ExportText(Text, &Source, nullptr, nullptr, PPF_None, nullptr);
	FVaRestJsonTestHolder Imported;
	HolderStruct->ImportText(*Text, &Imported, nullptr, PPF_None, GLog, HolderStruct->GetName());
	TestTrue(TEXT("astral and Cyrillic survive"), Imported.Json.Equals(Source.Json));
	TestEqual(TEXT("After not swallowed"), Imported.After, 7);
	TestEqual(TEXT("Before"), Imported.Before, 1);
	return true;
}

#endif
