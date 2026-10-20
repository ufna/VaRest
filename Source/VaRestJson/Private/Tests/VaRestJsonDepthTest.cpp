// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "VaRestJson.h"

namespace VaRestJsonDepthTest
{
	/** Levels nested arrays built through Add, which does not check depth on the way in (spec 2.2) */
	FVaRestJson NestedArrays(int32 Levels)
	{
		FVaRestJson Root = FVaRestJson::Array();
		FVaRestJson Current = Root;
		for (int32 i = 1; i < Levels; ++i)
		{
			Current.Add(FVaRestJson::Array());
			Current = Current[Current.Num() - 1];
		}
		return Root;
	}

	FString NestedText(int32 Levels)
	{
		return FString::ChrN(Levels, TEXT('[')) + FString::ChrN(Levels, TEXT(']'));
	}

	TSharedPtr<FJsonValue> NestedEngine(int32 Levels)
	{
		TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
		for (int32 i = 1; i < Levels; ++i)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Add(Value);
			Value = MakeShared<FJsonValueArray>(Items);
		}
		return Value;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDepthAtLimitTest, "VaRest.Json.Depth.AtLimitWorks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDepthAtLimitTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonDepthTest;
	const FString Text = NestedText(512);
	TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(FStringView(Text));
	if (!TestTrue(TEXT("512 levels parse"), Parsed.HasValue()))
	{
		return false;
	}
	const FVaRestJson J = Parsed.StealValue();
	TestEqualSensitive(TEXT("512 levels write back"), J.ToString(), Text);
	TestTrue(TEXT("TryToString succeeds"), J.TryToString().HasValue());
	const FVaRestJson Copy = J.Clone();
	TestTrue(TEXT("Clone at the limit"), Copy.IsArray() && Copy.Equals(J));
	const TSharedPtr<FJsonValue> Engine = J.ToJsonValue();
	TestTrue(TEXT("ToJsonValue at the limit"), Engine.IsValid());
	TestTrue(TEXT("FromJsonValue at the limit"), FVaRestJson::FromJsonValue(Engine).Equals(J));

	TArray<uint8> Saved;
	FMemoryWriter Writer(Saved);
	FVaRestJson ToSave = J;
	ToSave.Serialize(Writer);
	FMemoryReader Reader(Saved);
	FVaRestJson Loaded;
	Loaded.Serialize(Reader);
	TestTrue(TEXT("Serialize round trip at the limit"), Loaded.Equals(J));

	FVaRestJson Holder = FVaRestJson::Object();
	Holder.Set(TEXT("deep"), NestedArrays(512));
	TestTrue(TEXT("Set copies a 512-level value"), Holder.Has(TEXT("deep")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDepthPastLimitTest, "VaRest.Json.Depth.PastLimitFails",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDepthPastLimitTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonDepthTest;
	// One error line each from ToString, ToUtf8, Clone, Equals, ToJsonValue, FromJsonValue, FromJsonObject (cycle),
	// Serialize and Set; TryToString/TryToUtf8 and Parse report through their return value instead.
	AddExpectedErrorPlain(TEXT("deeper than 512"), EAutomationExpectedErrorFlags::Contains, 9);

	const FVaRestJson Deep = NestedArrays(513);
	TestEqualSensitive(TEXT("ToString past the limit is empty"), Deep.ToString(), FString());
	TArray<uint8> Out;
	Out.Add('x');
	Deep.ToUtf8(Out);
	TestEqual(TEXT("ToUtf8 appends nothing"), Out.Num(), 1);
	const TValueOrError<FString, FVaRestJsonError> Text = Deep.TryToString();
	TestTrue(TEXT("TryToString reports the depth"), Text.HasError() && Text.GetError().Message.Contains(TEXT("deeper than 512")));
	const TValueOrError<void, FVaRestJsonError> Bytes = Deep.TryToUtf8(Out);
	TestTrue(TEXT("TryToUtf8 reports and leaves Out as it was"), Bytes.HasError() && Out.Num() == 1);

	TestFalse(TEXT("Clone past the limit is invalid"), Deep.Clone().IsValid());
	TestFalse(TEXT("Equals past the limit is false"), Deep.Equals(NestedArrays(513)));
	TestFalse(TEXT("ToJsonValue past the limit is null"), Deep.ToJsonValue().IsValid());
	TestFalse(TEXT("FromJsonValue past the limit is invalid"), FVaRestJson::FromJsonValue(NestedEngine(513)).IsValid());

	// Review Focus: a cyclic engine graph is infinitely deep; it must hit the limit, not overflow the stack
	TSharedPtr<FJsonObject> Cycle = MakeShared<FJsonObject>();
	Cycle->SetObjectField(TEXT("self"), Cycle);
	TestFalse(TEXT("a cyclic FJsonObject is refused"), FVaRestJson::FromJsonObject(Cycle).IsValid());
	Cycle->RemoveField(TEXT("self")); // break the shared-pointer cycle so the object is freed

	TArray<uint8> Saved;
	FMemoryWriter Writer(Saved);
	FVaRestJson ToSave = Deep;
	ToSave.Serialize(Writer);
	FMemoryReader Reader(Saved);
	FVaRestJson Loaded;
	Loaded.Serialize(Reader);
	TestFalse(TEXT("a value too deep to save loads as an invalid handle"), Loaded.IsValid());
	TestFalse(TEXT("the archive itself is intact"), Reader.IsError());

	FVaRestJson Holder = FVaRestJson::Object();
	Holder.Set(TEXT("deep"), Deep);
	TestFalse(TEXT("Set of a value too deep to copy writes nothing"), Holder.Has(TEXT("deep")));

	// Parse clamps MaxDepth to 512, so anything that parses also writes
	FVaRestJsonParseOptions Generous;
	Generous.MaxDepth = 100000;
	const TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(FStringView(NestedText(513)), Generous);
	TestTrue(TEXT("a larger MaxDepth does not lift the limit"), Parsed.HasError() && Parsed.GetError().Message.Contains(TEXT("deeper than 512")));
	return true;
}

#endif
