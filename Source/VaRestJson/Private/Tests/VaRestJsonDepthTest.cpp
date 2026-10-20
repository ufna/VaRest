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
	/**
	 * Every depth guard has an array branch and an object branch (EmitNode, DeepCopyAt, EqualsAt, ToEngine, FromEngine),
	 * so each check runs on both shapes: [[[]]] and {"a":{"a":{}}}
	 */
	enum class EShape : uint8
	{
		Arrays,
		Objects
	};

	const TCHAR* ShapeName(EShape Shape)
	{
		return Shape == EShape::Arrays ? TEXT("arrays") : TEXT("objects");
	}

	/** Levels nested containers built through Add/Set, which do not check depth on the way in (spec 2.2) */
	FVaRestJson Nested(EShape Shape, int32 Levels)
	{
		if (Shape == EShape::Arrays)
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
		FVaRestJson Root = FVaRestJson::Object();
		FVaRestJson Current = Root;
		for (int32 i = 1; i < Levels; ++i)
		{
			Current.Set(TEXT("a"), FVaRestJson::Object());
			Current = Current[TEXT("a")];
		}
		return Root;
	}

	FString NestedText(EShape Shape, int32 Levels)
	{
		if (Shape == EShape::Arrays)
		{
			return FString::ChrN(Levels, TEXT('[')) + FString::ChrN(Levels, TEXT(']'));
		}
		FString Text;
		for (int32 i = 1; i < Levels; ++i)
		{
			Text += TEXT("{\"a\":");
		}
		return Text + TEXT("{}") + FString::ChrN(Levels - 1, TEXT('}'));
	}

	TSharedPtr<FJsonValue> NestedEngine(EShape Shape, int32 Levels)
	{
		if (Shape == EShape::Arrays)
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
		TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
		for (int32 i = 1; i < Levels; ++i)
		{
			TSharedPtr<FJsonObject> Outer = MakeShared<FJsonObject>();
			Outer->SetField(TEXT("a"), Value);
			Value = MakeShared<FJsonValueObject>(Outer);
		}
		return Value;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDepthAtLimitTest, "VaRest.Json.Depth.AtLimitWorks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDepthAtLimitTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonDepthTest;
	for (const EShape Shape : { EShape::Arrays, EShape::Objects })
	{
		const FString Of = FString::Printf(TEXT(" (%s)"), ShapeName(Shape));
		const FString Text = NestedText(Shape, 512);
		TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(FStringView(Text));
		if (!TestTrue(TEXT("512 levels parse") + Of, Parsed.HasValue()))
		{
			return false;
		}
		const FVaRestJson J = Parsed.StealValue();
		TestEqualSensitive(TEXT("512 levels write back") + Of, J.ToString(), Text);
		TestTrue(TEXT("TryToString succeeds") + Of, J.TryToString().HasValue());
		TestTrue(TEXT("512 levels built through the API equal the parsed ones") + Of, Nested(Shape, 512).Equals(J));
		const FVaRestJson Copy = J.Clone();
		TestTrue(TEXT("Clone at the limit") + Of, Copy.IsValid() && Copy.Equals(J));
		const TSharedPtr<FJsonValue> Engine = J.ToJsonValue();
		TestTrue(TEXT("ToJsonValue at the limit") + Of, Engine.IsValid());
		TestTrue(TEXT("FromJsonValue at the limit") + Of, FVaRestJson::FromJsonValue(Engine).Equals(J));
		TestTrue(TEXT("FromJsonValue of a 512-level engine tree") + Of, FVaRestJson::FromJsonValue(NestedEngine(Shape, 512)).Equals(J));

		TArray<uint8> Saved;
		FMemoryWriter Writer(Saved);
		FVaRestJson ToSave = J;
		ToSave.Serialize(Writer);
		FMemoryReader Reader(Saved);
		FVaRestJson Loaded;
		Loaded.Serialize(Reader);
		TestTrue(TEXT("Serialize round trip at the limit") + Of, Loaded.Equals(J));

		FVaRestJson Holder = FVaRestJson::Object();
		Holder.Set(TEXT("deep"), Nested(Shape, 512));
		TestTrue(TEXT("Set copies a 512-level value") + Of, Holder.Has(TEXT("deep")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDepthPastLimitTest, "VaRest.Json.Depth.PastLimitFails",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDepthPastLimitTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonDepthTest;
	// One error line per operation and shape, each attributed by its own message; TryToString/TryToUtf8 and Parse report
	// through their return value instead. FromJsonValue also refuses the cycle below, once.
	const int32 Shapes = 2;
	AddExpectedErrorPlain(TEXT("FVaRestJson::ToString: cannot write a value nested deeper than 512"), EAutomationExpectedErrorFlags::Contains, Shapes);
	AddExpectedErrorPlain(TEXT("FVaRestJson::ToUtf8: cannot write a value nested deeper than 512"), EAutomationExpectedErrorFlags::Contains, Shapes);
	AddExpectedErrorPlain(TEXT("FVaRestJson::Clone: cannot copy a value nested deeper than 512"), EAutomationExpectedErrorFlags::Contains, Shapes);
	AddExpectedErrorPlain(TEXT("FVaRestJson::Equals: cannot compare values nested deeper than 512"), EAutomationExpectedErrorFlags::Contains, Shapes);
	AddExpectedErrorPlain(TEXT("FVaRestJson::ToJsonValue: cannot convert a value nested deeper than 512"), EAutomationExpectedErrorFlags::Contains, Shapes);
	AddExpectedErrorPlain(TEXT("FVaRestJson::FromJsonValue: cannot convert a value nested deeper than 512"), EAutomationExpectedErrorFlags::Contains, Shapes + 1);
	AddExpectedErrorPlain(TEXT("FVaRestJson::Serialize: cannot save a value nested deeper than 512"), EAutomationExpectedErrorFlags::Contains, Shapes);
	AddExpectedErrorPlain(TEXT("FVaRestJson: cannot copy a value nested deeper than 512 levels; nothing was written"), EAutomationExpectedErrorFlags::Contains, Shapes);

	for (const EShape Shape : { EShape::Arrays, EShape::Objects })
	{
		const FString Of = FString::Printf(TEXT(" (%s)"), ShapeName(Shape));
		const FVaRestJson Deep = Nested(Shape, 513);
		TestEqualSensitive(TEXT("ToString past the limit is empty") + Of, Deep.ToString(), FString());
		TArray<uint8> Out;
		Out.Add('x');
		Deep.ToUtf8(Out);
		TestEqual(TEXT("ToUtf8 appends nothing") + Of, Out.Num(), 1);
		const TValueOrError<FString, FVaRestJsonError> Text = Deep.TryToString();
		TestTrue(TEXT("TryToString reports the depth") + Of, Text.HasError() && Text.GetError().Message.Contains(TEXT("deeper than 512")));
		const TValueOrError<void, FVaRestJsonError> Bytes = Deep.TryToUtf8(Out);
		TestTrue(TEXT("TryToUtf8 reports and leaves Out as it was") + Of, Bytes.HasError() && Out.Num() == 1);

		TestFalse(TEXT("Clone past the limit is invalid") + Of, Deep.Clone().IsValid());
		TestFalse(TEXT("Equals past the limit is false") + Of, Deep.Equals(Nested(Shape, 513)));
		TestFalse(TEXT("ToJsonValue past the limit is null") + Of, Deep.ToJsonValue().IsValid());
		TestFalse(TEXT("FromJsonValue past the limit is invalid") + Of, FVaRestJson::FromJsonValue(NestedEngine(Shape, 513)).IsValid());

		TArray<uint8> Saved;
		FMemoryWriter Writer(Saved);
		FVaRestJson ToSave = Deep;
		ToSave.Serialize(Writer);
		FMemoryReader Reader(Saved);
		FVaRestJson Loaded;
		Loaded.Serialize(Reader);
		TestFalse(TEXT("a value too deep to save loads as an invalid handle") + Of, Loaded.IsValid());
		TestFalse(TEXT("the archive itself is intact") + Of, Reader.IsError());

		FVaRestJson Holder = FVaRestJson::Object();
		Holder.Set(TEXT("deep"), Deep);
		TestFalse(TEXT("Set of a value too deep to copy writes nothing") + Of, Holder.Has(TEXT("deep")));

		// Parse clamps MaxDepth to 512, so anything that parses also writes
		FVaRestJsonParseOptions Generous;
		Generous.MaxDepth = 100000;
		const TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(FStringView(NestedText(Shape, 513)), Generous);
		TestTrue(TEXT("a larger MaxDepth does not lift the limit") + Of, Parsed.HasError() && Parsed.GetError().Message.Contains(TEXT("deeper than 512")));
	}

	// Review Focus: a cyclic engine graph is infinitely deep; it must hit the limit, not overflow the stack
	TSharedPtr<FJsonObject> Cycle = MakeShared<FJsonObject>();
	Cycle->SetObjectField(TEXT("self"), Cycle);
	TestFalse(TEXT("a cyclic FJsonObject is refused"), FVaRestJson::FromJsonObject(Cycle).IsValid());
	Cycle->RemoveField(TEXT("self")); // break the shared-pointer cycle so the object is freed
	return true;
}

#endif
