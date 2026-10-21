// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonConvertPrivate.h"
#include "VaRestJsonConvertTestTypes.h"
#include "VaRestJsonWriterInternal.h"

#include <limits>

namespace VaRestJsonConvertEncodeTest
{
	FString Utf8Text(const TArray<uint8>& Bytes)
	{
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
	}

	FVaRestJsonTestAll MakeFilled()
	{
		FVaRestJsonTestAll All;
		All.bIsActive = true;
		All.Int8Value = -8;
		All.Int16Value = -16;
		All.Int32Value = -32;
		All.Int64Value = MIN_int64;
		All.ByteValue = 255;
		All.Uint16Value = 65535;
		All.Uint32Value = MAX_uint32;
		All.Uint64Value = MAX_uint64;
		All.FloatValue = 0.1f;
		All.DoubleValue = 0.1;
		All.StringValue = TEXT("Ключ \"q\"");
		All.NameValue = FName(TEXT("Nm"));
		All.TextValue = FText::FromString(TEXT("Hello"));
		All.Color = EVaRestJsonTestColor::Green;
		All.Legacy = EVaRestJsonTestLegacy::Second;
		All.Inner.Id = 1;
		All.Inner.Name = TEXT("in");
		All.Numbers = { 1, 2, 3 };
		FVaRestJsonTestInner Item;
		Item.Id = 2;
		Item.Name = TEXT("two");
		All.Items.Add(Item);
		All.Tags.Add(TEXT("a"));
		All.Tags.Add(TEXT("b"));
		All.Scores.Add(TEXT("x"), 10);
		All.ById.Add(5, TEXT("five"));
		All.ByName.Add(FName(TEXT("n")), 2);
		All.ByColor.Add(EVaRestJsonTestColor::Blue, 3);
		All.Extra = FVaRestJson::Object().Set(TEXT("raw"), FVaRestJson::Array(1, 2));
		All.When = FDateTime(2026, 10, 20, 12, 34, 56, 789);
		All.Duration = FTimespan::FromSeconds(90.5);
		All.Guid = FGuid(0x01234567, 0x89ABCDEF, 0x01234567, 0x89ABCDEF);
		All.AssetPath = FSoftObjectPath(TEXT("/Game/Maps/Entry.Entry"));
		All.SoftAsset = TSoftObjectPtr<UObject>(FSoftObjectPath(TEXT("/Game/A.A")));
		All.Bytes = { 1, 2, 255 };
		All.MaybeNumber = 5;
		All.FixedArray[0] = 4;
		All.FixedArray[1] = 5;
		All.FixedArray[2] = 6;
		return All;
	}

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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeExactTest, "VaRest.Json.Convert.Encode.Exact",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeExactTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertEncodeTest;
	FVaRestJsonTestInner Inner;
	Inner.Id = 7;
	Inner.Name = TEXT("Neo");
	TArray<uint8> Bytes;
	Bytes.Add('>');
	TestTrue(TEXT("encodes"), FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestInner::StaticStruct(), &Inner, Bytes));
	TestEqualSensitive(TEXT("appends condensed UTF-8"), Utf8Text(Bytes), FString(TEXT(">{\"id\":7,\"name\":\"Neo\"}")));
	TestEqualSensitive(TEXT("pretty"), FVaRestJsonConvert::StructToString(FVaRestJsonTestInner::StaticStruct(), &Inner, FVaRestJsonConvertOptions(), EVaRestJsonFormat::Pretty),
		FString(TEXT("{\n  \"id\": 7,\n  \"name\": \"Neo\"\n}")));
	TestEqualSensitive(TEXT("ToUtf8<T>"), Utf8Text(FVaRestJsonConvert::ToUtf8(Inner)), FString(TEXT("{\"id\":7,\"name\":\"Neo\"}")));
	TestEqualSensitive(TEXT("ToJson<T>"), FVaRestJsonConvert::ToJson(Inner).ToString(), FString(TEXT("{\"id\":7,\"name\":\"Neo\"}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeMembersTest, "VaRest.Json.Convert.Encode.MembersIntoOpenWriter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeMembersTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json;
	using namespace VaRestJsonConvertEncodeTest;
	// What a socket layer does (WebSocket spec §19 item 9): its own envelope, the struct's members inside, one pass
	FVaRestJsonTestInner Inner;
	Inner.Id = 7;
	Inner.Name = TEXT("Neo");
	TArray<uint8> Bytes;
	FUtf8Sink Sink{ Bytes };
	FWriterAllocator StackAllocator;
	FCondensedWriter Writer(Sink, &StackAllocator);
	Writer.StartObject();
	Writer.Key("type");
	Writer.String("hello");
	Writer.Key("data");
	Writer.StartObject();
	TestTrue(TEXT("members written"), WriteStructMembersInternal(Writer, FVaRestJsonTestInner::StaticStruct(), &Inner, FVaRestJsonConvertOptions(), 2));
	Writer.EndObject();
	Writer.EndObject();
	TestEqualSensitive(TEXT("one-pass envelope"), Utf8Text(Bytes), FString(TEXT("{\"type\":\"hello\",\"data\":{\"id\":7,\"name\":\"Neo\"}}")));

	// A struct with a scalar JSON form has no members to write
	TArray<uint8> Other;
	FUtf8Sink OtherSink{ Other };
	FCondensedWriter OtherWriter(OtherSink, &StackAllocator);
	OtherWriter.StartObject();
	const FGuid Guid;
	FVaRestJsonError Error;
	TestFalse(TEXT("a GUID has no members"), WriteStructMembersInternal(OtherWriter, TBaseStructure<FGuid>::Get(), &Guid, FVaRestJsonConvertOptions(), 1, &Error));
	TestTrue(TEXT("... and says so"), Error.Message.Contains(TEXT("has no members")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeAllTypesTest, "VaRest.Json.Convert.Encode.AllTypes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeAllTypesTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertEncodeTest;
	const FVaRestJsonTestAll All = MakeFilled();
	const FVaRestJson J = FVaRestJsonConvert::StructToJson(FVaRestJsonTestAll::StaticStruct(), &All);
	if (!TestTrue(TEXT("encoded"), J.IsObject()))
	{
		return false;
	}
	TestTrue(TEXT("bool, 'b' dropped"), J[TEXT("isActive")].IsBool() && J[TEXT("isActive")].AsBool());
	TestEqual(TEXT("int8"), J[TEXT("int8Value")].AsInt(), -8);
	TestEqual(TEXT("int16"), J[TEXT("int16Value")].AsInt(), -16);
	TestEqual(TEXT("int64 min exact"), J[TEXT("int64Value")].AsInt64(), MIN_int64);
	TestEqual(TEXT("byte"), J[TEXT("byteValue")].AsInt(), 255);
	TestEqual(TEXT("uint32 max"), J[TEXT("uint32Value")].AsInt64(), static_cast<int64>(MAX_uint32));
	TestTrue(TEXT("uint64 max exact"), J[TEXT("uint64Value")].AsUInt64() == MAX_uint64);
	TestTrue(TEXT("float written shortest"), J[TEXT("floatValue")].AsDouble() == 0.1);
	TestTrue(TEXT("double"), J[TEXT("doubleValue")].AsDouble() == 0.1);
	TestEqualSensitive(TEXT("string"), J[TEXT("stringValue")].AsString(), FString(TEXT("Ключ \"q\"")));
	TestEqualSensitive(TEXT("name"), J[TEXT("nameValue")].AsString(), FString(TEXT("Nm")));
	TestEqualSensitive(TEXT("text"), J[TEXT("textValue")].AsString(), FString(TEXT("Hello")));
	TestEqualSensitive(TEXT("enum class by name"), J[TEXT("color")].AsString(), FString(TEXT("Green")));
	TestEqualSensitive(TEXT("byte enum by name"), J[TEXT("legacy")].AsString(), FString(TEXT("Second")));
	TestEqual(TEXT("nested struct"), J.Path(TEXT("inner.id")).AsInt(), 1);
	TestEqualSensitive(TEXT("numbers"), J[TEXT("numbers")].ToString(), FString(TEXT("[1,2,3]")));
	TestEqualSensitive(TEXT("array of structs"), J[TEXT("items")].ToString(), FString(TEXT("[{\"id\":2,\"name\":\"two\"}]")));
	TestEqual(TEXT("set as array"), J[TEXT("tags")].Num(), 2);
	TestEqual(TEXT("string-keyed map"), J.Path(TEXT("scores.x")).AsInt(), 10);
	TestEqualSensitive(TEXT("int-keyed map"), J[TEXT("byId")].ToString(), FString(TEXT("{\"5\":\"five\"}")));
	TestEqual(TEXT("name-keyed map"), J.Path(TEXT("byName.n")).AsInt(), 2);
	TestEqual(TEXT("enum-keyed map"), J.Path(TEXT("byColor.Blue")).AsInt(), 3);
	TestEqualSensitive(TEXT("FVaRestJson passthrough"), J[TEXT("extra")].ToString(), FString(TEXT("{\"raw\":[1,2]}")));
	TestEqualSensitive(TEXT("ISO 8601"), J[TEXT("when")].AsString(), FString(TEXT("2026-10-20T12:34:56.789Z")));
	TestTrue(TEXT("timespan in seconds"), J[TEXT("duration")].AsDouble() == 90.5);
	TestEqualSensitive(TEXT("guid"), J[TEXT("guid")].AsString(), FString(TEXT("01234567-89AB-CDEF-0123-456789ABCDEF")));
	TestEqualSensitive(TEXT("soft path"), J[TEXT("assetPath")].AsString(), FString(TEXT("/Game/Maps/Entry.Entry")));
	TestEqualSensitive(TEXT("soft object"), J[TEXT("softAsset")].AsString(), FString(TEXT("/Game/A.A")));
	TestEqualSensitive(TEXT("bytes as numbers"), J[TEXT("bytes")].ToString(), FString(TEXT("[1,2,255]")));
	TestEqual(TEXT("optional set"), J[TEXT("maybeNumber")].AsInt(), 5);
	TestEqualSensitive(TEXT("C-style array"), J[TEXT("fixedArray")].ToString(), FString(TEXT("[4,5,6]")));
	TestEqual(TEXT("field count"), J.Num(), 33);

	// Both sinks write the same text
	TestEqualSensitive(TEXT("bytes path == DOM path"), FVaRestJsonConvert::StructToString(FVaRestJsonTestAll::StaticStruct(), &All), J.ToString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeOptionsTest, "VaRest.Json.Convert.Encode.Options",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeOptionsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonConvertEncodeTest;
	FVaRestJsonTestAll All = MakeFilled();
	All.MaybeNumber.Reset();
	All.Extra = FVaRestJson();
	FVaRestJsonConvertOptions Options;
	Options.bEnumsAsNumbers = true;
	Options.bBytesAsBase64 = true;
	Options.Naming = EVaRestJsonNaming::SnakeCase;
	const FVaRestJson J = FVaRestJsonConvert::StructToJson(FVaRestJsonTestAll::StaticStruct(), &All, Options);
	TestEqual(TEXT("enum as number"), J[TEXT("color")].AsInt(), 1);
	TestEqual(TEXT("byte enum as number"), J[TEXT("legacy")].AsInt(), 1);
	TestEqual(TEXT("enum map key as number"), J.Path(TEXT("by_color.2")).AsInt(), 3);
	TestEqualSensitive(TEXT("bytes as base64"), J[TEXT("bytes")].AsString(), FString(TEXT("AQL/")));
	TestFalse(TEXT("an unset optional is omitted"), J.Has(TEXT("maybe_number")));
	TestFalse(TEXT("a missing FVaRestJson is omitted"), J.Has(TEXT("extra")));
	TestTrue(TEXT("snake_case names"), J.Has(TEXT("is_active")) && J.Has(TEXT("int64_value")) && J.Has(TEXT("by_id")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeDepthTest, "VaRest.Json.Convert.Encode.Depth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeDepthTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json;
	using namespace VaRestJsonConvertEncodeTest;
	// The struct's object and its Children array are two levels: 511 nested arrays inside make 513 containers
	FVaRestJsonTestTree Deep;
	Deep.Children.Add(NestedArrays(511));
	AddExpectedErrorPlain(TEXT("deeper than 512"), EAutomationExpectedErrorFlags::Contains, 2);
	TArray<uint8> Out;
	Out.Add('x');
	TestFalse(TEXT("too deep to encode"), FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestTree::StaticStruct(), &Deep, Out));
	TestEqual(TEXT("Out unchanged"), Out.Num(), 1);
	TestFalse(TEXT("the DOM path fails too"), FVaRestJsonConvert::StructToJson(FVaRestJsonTestTree::StaticStruct(), &Deep).IsValid());
	FVaRestJsonError Error;
	TestFalse(TEXT("with an error out-param nothing is logged"), FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestTree::StaticStruct(), &Deep, Out, FVaRestJsonConvertOptions(), EVaRestJsonFormat::Condensed, &Error));
	TestTrue(TEXT("the error says why"), Error.Message.Contains(TEXT("deeper than 512")));

	// 512 containers: what the parser accepts (VaRest.Json.Depth.AtLimitWorks), so anything that parses also encodes
	FVaRestJsonTestTree Fine;
	Fine.Children.Add(NestedArrays(510));
	TestTrue(TEXT("MaxDepth containers encode"), FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestTree::StaticStruct(), &Fine, Out));
	TestTrue(TEXT("... on the DOM path too"), FVaRestJsonConvert::StructToJson(FVaRestJsonTestTree::StaticStruct(), &Fine).IsValid());

	// The walker's own containers count on top of the caller's open ones (the writer entries)
	const FVaRestJsonTestTree Empty; // an object and an empty array
	FWriterAllocator StackAllocator;
	TArray<uint8> AtLimitBytes;
	FUtf8Sink AtLimitSink{ AtLimitBytes };
	FCondensedWriter AtLimitWriter(AtLimitSink, &StackAllocator);
	TestTrue(TEXT("two levels inside MaxDepth - 2 open containers"), WriteStructInternal(AtLimitWriter, FVaRestJsonTestTree::StaticStruct(), &Empty, FVaRestJsonConvertOptions(), MaxDepth - 2));
	TArray<uint8> PastLimitBytes;
	FUtf8Sink PastLimitSink{ PastLimitBytes };
	FCondensedWriter PastLimitWriter(PastLimitSink, &StackAllocator);
	Error = FVaRestJsonError();
	TestFalse(TEXT("... but not inside MaxDepth - 1"), WriteStructInternal(PastLimitWriter, FVaRestJsonTestTree::StaticStruct(), &Empty, FVaRestJsonConvertOptions(), MaxDepth - 1, &Error));
	TestTrue(TEXT("... with the depth error"), Error.Message.Contains(TEXT("deeper than 512")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeFloatTest, "VaRest.Json.Convert.Encode.FloatShortest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeFloatTest::RunTest(const FString& Parameters)
{
	using VaRest::Json::Convert::FloatToJsonDouble;
	const float Samples[] = { 0.1f, 1.5f, 3.14159f, 1e-7f, 123456.79f, 16777216.f, TNumericLimits<float>::Max(), -2.5e-3f, TNumericLimits<float>::Min() };
	for (const float Sample : Samples)
	{
		TestTrue(FString::Printf(TEXT("%g reads back as the same float"), Sample), static_cast<float>(FloatToJsonDouble(Sample)) == Sample);
	}
	TestEqualSensitive(TEXT("0.1f is written 0.1"), FVaRestJson::Object().Set(TEXT("f"), FloatToJsonDouble(0.1f)).ToString(), FString(TEXT("{\"f\":0.1}")));
	TestEqualSensitive(TEXT("1.5f is written 1.5"), FVaRestJson::Object().Set(TEXT("f"), FloatToJsonDouble(1.5f)).ToString(), FString(TEXT("{\"f\":1.5}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeSkippedTest, "VaRest.Json.Convert.Encode.SkippedAndNonFinite",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeSkippedTest::RunTest(const FString& Parameters)
{
	FVaRestJsonConvert::ResetCache();
	AddExpectedMessagePlain(TEXT("VaRestJsonTestSkipped."), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 3);
	FVaRestJsonTestSkipped Skipped;
	Skipped.Kept = 4;
	TestEqualSensitive(TEXT("only convertible fields"), FVaRestJsonConvert::StructToString(FVaRestJsonTestSkipped::StaticStruct(), &Skipped), FString(TEXT("{\"kept\":4}")));

	AddExpectedMessagePlain(TEXT("non-finite"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FVaRestJsonTestAll All;
	All.DoubleValue = std::numeric_limits<double>::quiet_NaN();
	const FVaRestJson J = FVaRestJsonConvert::StructToJson(FVaRestJsonTestAll::StaticStruct(), &All);
	TestTrue(TEXT("NaN written as null"), J[TEXT("doubleValue")].IsNull());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeBytesWriterTest, "VaRest.Json.Convert.Encode.BytesWriter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeBytesWriterTest::RunTest(const FString& Parameters)
{
	// Socket.IO spec §18.2 J-1: binary values become numbered placeholders, written in place in one pass
	FVaRestJsonTestAttachments Value;
	Value.First = { 1, 2 };
	Value.List.AddDefaulted(2);
	Value.List[0].Data = { 3 }; // List[1].Data stays empty: an empty array is offered too
	Value.ByName.Add(TEXT("k")).Data = { 4, 5, 6 };

	TArray<int32> Seen; // the sizes the hook was offered, in walk order
	FVaRestJsonConvertOptions Options;
	Options.BytesWriter = [&Seen](TConstArrayView<uint8> Bytes, FVaRestJson& OutReplacement)
	{
		OutReplacement = FVaRestJson::Object().Set(TEXT("_placeholder"), true).Set(TEXT("num"), Seen.Num());
		Seen.Add(Bytes.Num());
		return true;
	};
	const FString Text = FVaRestJsonConvert::StructToString(FVaRestJsonTestAttachments::StaticStruct(), &Value, Options);
	TestEqualSensitive(TEXT("replacements in place, numbered in walk order"), Text, FString(TEXT(
		"{\"first\":{\"_placeholder\":true,\"num\":0},\"list\":[{\"data\":{\"_placeholder\":true,\"num\":1}},{\"data\":{\"_placeholder\":true,\"num\":2}}],"
		"\"byName\":{\"k\":{\"data\":{\"_placeholder\":true,\"num\":3}}}}")));
	TestTrue(TEXT("the hook saw every value's bytes, the empty one too"), Seen == TArray<int32>({ 2, 1, 0, 3 }));

	Seen.Reset();
	TestEqualSensitive(TEXT("the DOM path agrees"), FVaRestJsonConvert::StructToJson(FVaRestJsonTestAttachments::StaticStruct(), &Value, Options).ToString(), Text);

	FVaRestJsonConvertOptions Declining;
	Declining.BytesWriter = [](TConstArrayView<uint8> Bytes, FVaRestJson& OutReplacement) { return false; };
	TestEqualSensitive(TEXT("a declining hook leaves the built-in form"), FVaRestJsonConvert::StructToString(FVaRestJsonTestAttachments::StaticStruct(), &Value, Declining),
		FString(TEXT("{\"first\":[1,2],\"list\":[{\"data\":[3]},{\"data\":[]}],\"byName\":{\"k\":{\"data\":[4,5,6]}}}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeWriterEntriesTest, "VaRest.Json.Convert.Encode.WriterEntries",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeWriterEntriesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json;
	using namespace VaRestJsonConvertEncodeTest;
	// Socket.IO positional arguments (§18.2 J-2) and WebSocket's Emit(Route, FVaRestJson) (§6.1), into one open array
	FVaRestJsonTestInner Inner;
	Inner.Id = 7;
	Inner.Name = TEXT("n");
	const FDateTime When(2026, 10, 20, 12, 0, 0);
	const FVaRestJson Payload = FVaRestJson::Object().Set(TEXT("a"), 1).Set(TEXT("b"), FVaRestJson::Array(true, nullptr));
	const FProperty* NameProperty = FindFProperty<FProperty>(FVaRestJsonTestInner::StaticStruct(), FName(TEXT("Name")));
	const void* NameValue = NameProperty->ContainerPtrToValuePtr<void>(&Inner);

	TArray<uint8> Bytes;
	FUtf8Sink Sink{ Bytes };
	FWriterAllocator StackAllocator;
	FCondensedWriter Writer(Sink, &StackAllocator);
	Writer.StartArray();
	TestTrue(TEXT("WriteJsonInternal"), WriteJsonInternal(Writer, Payload, 1));
	TestTrue(TEXT("an invalid handle writes null"), WriteJsonInternal(Writer, FVaRestJson(), 1));
	TestTrue(TEXT("WriteStructInternal, an object"), WriteStructInternal(Writer, FVaRestJsonTestInner::StaticStruct(), &Inner, FVaRestJsonConvertOptions(), 1));
	TestTrue(TEXT("WriteStructInternal, a value struct"), WriteStructInternal(Writer, TBaseStructure<FDateTime>::Get(), &When, FVaRestJsonConvertOptions(), 1));
	TestTrue(TEXT("WritePropertyInternal"), WritePropertyInternal(Writer, NameProperty, NameValue, FVaRestJsonConvertOptions(), 1));
	Writer.StartObject();
	TestTrue(TEXT("WriteJsonMembersInternal"), WriteJsonMembersInternal(Writer, Payload, 2));
	Writer.EndObject();
	Writer.EndArray();
	const FString Expected = FString::Printf(TEXT("[%s,null,%s,\"%s\",%s,%s]"), *Payload.ToString(),
		*FVaRestJsonConvert::StructToString(FVaRestJsonTestInner::StaticStruct(), &Inner), *When.ToIso8601(),
		*FVaRestJsonConvert::PropertyToJson(NameProperty, NameValue).ToString(), *Payload.ToString());
	TestEqualSensitive(TEXT("each entry writes what the DOM path builds"), Utf8Text(Bytes), Expected);

	// The depth limit counts the caller's open containers; OutError keeps the failure out of the log
	FVaRestJsonError Error;
	TArray<uint8> DeepBytes;
	FUtf8Sink DeepSink{ DeepBytes };
	FCondensedWriter DeepWriter(DeepSink, &StackAllocator);
	TestFalse(TEXT("an object inside MaxDepth open containers fails"), WriteJsonInternal(DeepWriter, Payload, MaxDepth, &Error));
	TestTrue(TEXT("... with the depth error"), Error.Message.Contains(TEXT("deeper than")));
	TArray<uint8> ScalarBytes;
	FUtf8Sink ScalarSink{ ScalarBytes };
	FCondensedWriter ScalarWriter(ScalarSink, &StackAllocator);
	TestTrue(TEXT("a scalar there is fine"), WriteJsonInternal(ScalarWriter, FVaRestJson::From(1), MaxDepth));
	TArray<uint8> NotObjectBytes;
	FUtf8Sink NotObjectSink{ NotObjectBytes };
	FCondensedWriter NotObjectWriter(NotObjectSink, &StackAllocator);
	TestFalse(TEXT("members of a non-object"), WriteJsonMembersInternal(NotObjectWriter, FVaRestJson::Array(1), 1, &Error));

	// Positional arguments (J-2) iterate GetConvertedFieldsInternal: the fields StructToUtf8 writes, in its order. Not
	// TFieldIterator: that also yields the transient, deprecated and editor-only fields (the last exists in PIE only, so
	// every later argument would shift in a packaged game) and the unsupported one, which StructToUtf8 skips.
	FVaRestJsonConvert::ResetCache(); // one field-map build below, so one skip warning
	AddExpectedMessagePlain(TEXT("VaRestJsonTestPositional.PosObject is skipped"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FVaRestJsonTestPositional Args;
	Args.PosFirst = 3;
	Args.PosTransient = 4;
	Args.PosOld_DEPRECATED = 5;
#if WITH_EDITORONLY_DATA
	Args.PosEditorOnly = 6;
#endif
	Args.PosLast = TEXT("z");
	const UScriptStruct* ArgsStruct = FVaRestJsonTestPositional::StaticStruct();
	const TArray<const FProperty*> Fields = GetConvertedFieldsInternal(ArgsStruct);
	TArray<FString> FieldNames;
	for (const FProperty* Field : Fields)
	{
		FieldNames.Add(Field->GetName());
	}
	TestEqualSensitive(TEXT("the converted fields, in declaration order"), FString::Join(FieldNames, TEXT(",")), FString(TEXT("PosFirst,PosLast")));

	TArray<uint8> ArgBytes;
	FUtf8Sink ArgSink{ ArgBytes };
	FCondensedWriter ArgWriter(ArgSink, &StackAllocator);
	ArgWriter.StartArray();
	bool bAllWritten = true;
	for (const FProperty* Field : Fields)
	{
		bAllWritten &= WritePropertyInternal(ArgWriter, Field, Field->ContainerPtrToValuePtr<void>(&Args), FVaRestJsonConvertOptions(), 1);
	}
	// After a false write the writer is unusable (the contract in VaRestJsonWriterInternal.h): close the array only when
	// every write succeeded, so a field that fails part-way fails this test instead of a check
	if (bAllWritten)
	{
		ArgWriter.EndArray();
	}
	TestTrue(TEXT("every converted field writes"), bAllWritten);

	TArray<uint8> ObjectBytes;
	TestTrue(TEXT("StructToUtf8"), FVaRestJsonConvert::StructToUtf8(ArgsStruct, &Args, ObjectBytes));
	const FVaRestJson Written = FVaRestJson::Parse(TConstArrayView<uint8>(ObjectBytes)).StealValue();
	FVaRestJson WrittenValues = FVaRestJson::Array();
	for (const FVaRestJsonMember& Member : Written.Members())
	{
		WrittenValues.Add(Member.Value);
	}
	TestEqualSensitive(TEXT("StructToUtf8 writes the two converted fields"), Utf8Text(ObjectBytes), FString(TEXT("{\"posFirst\":3,\"posLast\":\"z\"}")));
	TestEqualSensitive(TEXT("the positional arguments are StructToUtf8's values, in its order"), Utf8Text(ArgBytes), WrittenValues.ToString());
	return true;
}

#endif
