// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Algo/Reverse.h"
#include "Misc/AutomationTest.h"
#include "UObject/EnumProperty.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonConvertTestTypes.h"
#include "VaRestJsonWriterInternal.h"

namespace VaRestJsonRulesTest
{
struct FOwner
{
	// The owner is only a real weak UObject identity; UObject itself is abstract on this SDK.
	TStrongObjectPtr<UObject> Object{NewObject<UEnum>()};
	~FOwner() { FVaRestJsonRules::UnregisterOwner(Object.Get()); }
	const UObject* Get() const { return Object.Get(); }
};

TArray<FVaRestJsonFieldRule> Fields()
{
	TArray<FVaRestJsonFieldRule> Rules;
	const TCHAR* Names[] = {TEXT("ExactCase"), TEXT("omit"), TEXT("nullableDefault"), TEXT("decode"),
		TEXT("ignore"), TEXT("bytes"), TEXT("values"), TEXT("color")};
	for (int32 i = 0; i < UE_ARRAY_COUNT(Names); ++i)
	{
		FVaRestJsonFieldRule Rule;
		Rule.Field = FGuid(0, 0, 0, i + 1);
		Rule.Json = Names[i];
		Rules.Add(MoveTemp(Rule));
	}
	Rules[1].Presence = EVaRestJsonPresence::OmitWhenDefault;
	Rules[2].Presence = EVaRestJsonPresence::NullWhenDefault;
	Rules[3].Presence = EVaRestJsonPresence::DecodeOnly;
	Rules[4].Presence = EVaRestJsonPresence::Ignore;
	Rules[5].bBase64 = true;
	Rules[6].bNullable = true;
	Rules[7].Presence = EVaRestJsonPresence::NullWhenDefault;
	return Rules;
}

TArray<FVaRestJsonEnumRule> Enums()
{
	FVaRestJsonEnumRule Red;
	Red.Enumerator = TEXT("Red");
	Red.Json = TEXT("red-wire");
	FVaRestJsonEnumRule Green;
	Green.Enumerator = TEXT("Green");
	Green.Json = TEXT("green-wire");
	FVaRestJsonEnumRule Blue;
	Blue.Enumerator = TEXT("Blue");
	Blue.bUnset = true;
	return {Red, Green, Blue};
}
} // namespace VaRestJsonRulesTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRulesOwnersTest, "VaRest.Json.Rules.OwnersAndSnapshots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonRulesOwnersTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonRulesTest;
	FOwner A;
	FOwner B;
	const UScriptStruct* Type = FVaRestJsonTestRules::StaticStruct();
	TArray<FVaRestJsonFieldRule> Rules = Fields();
	TestTrue(TEXT("register A"), FVaRestJsonRules::RegisterFields(A.Get(), Type, Rules));
	const auto Old = FVaRestJsonRules::Snapshot(A.Get());
	TestEqual(TEXT("an unchanged owner reuses one snapshot/cache identity"), Old->GetId(), FVaRestJsonRules::Snapshot(A.Get())->GetId());
	Algo::Reverse(Rules);
	TestTrue(TEXT("equal declarations in a different order coexist"), FVaRestJsonRules::RegisterFields(B.Get(), Type, Rules));
	TestTrue(TEXT("no last-writer conflict for equal rules"), FVaRestJsonRules::DefaultSnapshot()->Find(Type)->Error.IsEmpty());
	Rules.Last().Json = TEXT("other-key");
	TestTrue(TEXT("conflicting registration is retained as an explicit owner view"), FVaRestJsonRules::RegisterFields(B.Get(), Type, Rules));
	TestFalse(TEXT("default refuses the conflicted type"), FVaRestJsonRules::DefaultSnapshot()->Find(Type)->Error.IsEmpty());
	TestTrue(TEXT("explicit owner A overrides its own conflict"), FVaRestJsonRules::Snapshot(A.Get())->Find(Type)->Error.IsEmpty());
	FVaRestJsonTestRules Value;
	FVaRestJsonError Error;
	TArray<uint8> Bytes{42};
	TestFalse(TEXT("default conversion cannot choose the most recent owner"), FVaRestJsonConvert::StructToUtf8(Type, &Value, Bytes, {}, EVaRestJsonFormat::Condensed, &Error));
	TestEqual(TEXT("refused conversion preserves caller output"), Bytes.Num(), 1);
	TestTrue(TEXT("conflict names the concrete type"), Error.Message.Contains(Type->GetPathName()));
	TArray<const FProperty*> Positional;
	TestFalse(TEXT("positional discovery propagates conflict before emitting arguments"), VaRest::Json::TryGetConvertedFieldsInternal(Type, Positional, {}, &Error));
	TestTrue(TEXT("refused positional discovery has no fields"), Positional.IsEmpty());
	FVaRestJsonConvertOptions Options;
	Options.Rules = Old;
	TestEqual(TEXT("an earlier immutable snapshot still encodes its old key"), FVaRestJsonConvert::ToJson(Value, Options)[TEXT("ExactCase")].AsInt(), 7);
	FVaRestJsonRules::UnregisterOwner(B.Get());
	TestEqual(TEXT("removing B preserves A/default rules"), FVaRestJsonConvert::ToJson(Value)[TEXT("ExactCase")].AsInt(), 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRulesFieldsTest, "VaRest.Json.Rules.FieldsAndDefaults",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonRulesFieldsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonRulesTest;
	FOwner Owner;
	TestTrue(TEXT("register fields"), FVaRestJsonRules::RegisterFields(Owner.Get(), FVaRestJsonTestRules::StaticStruct(), Fields()));
	FVaRestJsonTestRules Value;
	Value.Bytes_6_00000000000000000000000000000006 = {0, 1, 255};
	const FVaRestJson Json = FVaRestJsonConvert::ToJson(Value);
	TestEqual(TEXT("GUID supplies an exact case-sensitive wire key"), Json[TEXT("ExactCase")].AsInt(), 7);
	TestFalse(TEXT("authored default is omitted"), Json[TEXT("omit")].IsValid());
	TestTrue(TEXT("authored default becomes null"), Json[TEXT("nullableDefault")].IsNull());
	TestFalse(TEXT("decode-only is not encoded"), Json[TEXT("decode")].IsValid());
	TestFalse(TEXT("ignored field is not encoded"), Json[TEXT("ignore")].IsValid());
	TestEqualSensitive(TEXT("field base64"), Json[TEXT("bytes")].AsString(), FString(TEXT("AAH/")));
	Value.Omit_2_00000000000000000000000000000002 = 10;
	TestEqual(TEXT("a non-default value is retained"), FVaRestJsonConvert::ToJson(Value)[TEXT("omit")].AsInt(), 10);
	Value.Values_7_00000000000000000000000000000007 = {3, 4};
	FVaRestJsonDecodeReport Report;
	const FVaRestJson Input = FVaRestJson::Object().Set(TEXT("decode"), 20).Set(TEXT("ignore"), 99).Set(TEXT("values"), FVaRestJson::Null());
	TestTrue(TEXT("decode rules"), FVaRestJsonConvert::JsonToStruct(Input, FVaRestJsonTestRules::StaticStruct(), &Value, {}, &Report));
	TestEqual(TEXT("decode-only accepts input"), Value.Decode_4_00000000000000000000000000000004, 20);
	TestEqual(TEXT("ignored field keeps its current value"), Value.Ignore_5_00000000000000000000000000000005, 15);
	TestEqual(TEXT("nullable null preserves a container"), Value.Values_7_00000000000000000000000000000007.Num(), 2);
	FVaRestJsonConvertOptions Options;
	Options.BytesWriter = [](TConstArrayView<uint8>, FVaRestJson& Replacement) {
		Replacement = FVaRestJson::From(TEXT("attachment"));
		return true;
	};
	TestEqualSensitive(TEXT("attachment writer precedes field base64"), FVaRestJsonConvert::ToJson(Value, Options)[TEXT("bytes")].AsString(), FString(TEXT("attachment")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRulesEnumsTest, "VaRest.Json.Rules.EnumWireValuesAndUnset",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonRulesEnumsTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonRulesTest;
	FOwner Owner;
	TestTrue(TEXT("register enum"), FVaRestJsonRules::RegisterEnum(Owner.Get(), StaticEnum<EVaRestJsonTestColor>(), Enums()));
	TestTrue(TEXT("register fields"), FVaRestJsonRules::RegisterFields(Owner.Get(), FVaRestJsonTestRules::StaticStruct(), Fields()));
	FVaRestJsonTestRules Value;
	Value.Colors = {EVaRestJsonTestColor::Blue, EVaRestJsonTestColor::Red, EVaRestJsonTestColor::Green};
	Value.ColorByName.Add(TEXT("skip"), EVaRestJsonTestColor::Blue);
	Value.ColorByName.Add(TEXT("keep"), EVaRestJsonTestColor::Red);
	Value.FixedColors[0] = EVaRestJsonTestColor::Blue;
	Value.FixedColors[1] = EVaRestJsonTestColor::Red;
	AddExpectedMessagePlain(TEXT("Unset enum entry omitted"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 6);
	const FVaRestJson Json = FVaRestJsonConvert::ToJson(Value);
	TestTrue(TEXT("unset object value uses NullWhenDefault"), Json[TEXT("color")].IsNull());
	TestEqualSensitive(TEXT("unset array entry is omitted; mapped order retained"), Json[TEXT("colors")].ToString(), FString(TEXT("[\"red-wire\",\"green-wire\"]")));
	TestEqualSensitive(TEXT("unset map entry is omitted"), Json[TEXT("colorByName")].ToString(), FString(TEXT("{\"keep\":\"red-wire\"}")));
	TestEqualSensitive(TEXT("fixed array has the same omission semantics"), Json[TEXT("fixedColors")].ToString(), FString(TEXT("[\"red-wire\"]")));
	TArray<uint8> Encoded;
	FVaRestJsonError EncodeError;
	if (TestTrue(TEXT("fixed-array UTF-8 encode"), FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestRules::StaticStruct(), &Value, Encoded, {}, EVaRestJsonFormat::Condensed, &EncodeError)))
	{
		auto Parsed = FVaRestJson::Parse(TConstArrayView<uint8>(Encoded));
		if (TestTrue(TEXT("encoded UTF-8 is valid JSON"), Parsed.HasValue()))
		{
			TestEqualSensitive(TEXT("UTF-8 and DOM fixed-array behavior agree"), Parsed.GetValue()[TEXT("fixedColors")].ToString(), FString(TEXT("[\"red-wire\"]")));
		}
	}
	FVaRestJsonDecodeReport Report;
	TestTrue(TEXT("mapped wire decode"), FVaRestJsonConvert::JsonToStruct(FVaRestJson::Object().Set(TEXT("color"), TEXT("green-wire")), FVaRestJsonTestRules::StaticStruct(), &Value, {}, &Report));
	TestEqual(TEXT("mapped enum result"), Value.Color_8_00000000000000000000000000000008, EVaRestJsonTestColor::Green);
	FVaRestJsonConvertOptions Strict;
	Strict.bStrict = true;
	TestFalse(TEXT("unset has no authored-name reverse match"), FVaRestJsonConvert::JsonToStruct(FVaRestJson::Object().Set(TEXT("color"), TEXT("Blue")), FVaRestJsonTestRules::StaticStruct(), &Value, Strict, &Report));
	TestEqual(TEXT("refused enum leaves current value"), Value.Color_8_00000000000000000000000000000008, EVaRestJsonTestColor::Green);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRulesExactWireTest, "VaRest.Json.Rules.ExactWireNamespace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonRulesExactWireTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonRulesTest;
	FOwner A;
	FOwner B;
	const UScriptStruct* Type = FVaRestJsonTestRules::StaticStruct();
	auto Rules = Fields();
	TestTrue(TEXT("register exact field name"), FVaRestJsonRules::RegisterFields(A.Get(), Type, Rules));
	Rules[0].Json = TEXT("exactcase");
	TestTrue(TEXT("register case-only different declaration"), FVaRestJsonRules::RegisterFields(B.Get(), Type, Rules));
	TestFalse(TEXT("case-only owner declarations conflict"), FVaRestJsonRules::DefaultSnapshot()->Find(Type)->Error.IsEmpty());
	FVaRestJsonRules::UnregisterOwner(B.Get());
	Rules[0].Json = TEXT("Wire");
	Rules[1].Json = TEXT("wire");
	TestTrue(TEXT("case-distinct keys within one owner are valid"), FVaRestJsonRules::RegisterFields(A.Get(), Type, Rules));
	FVaRestJsonTestRules Value;
	Value.Omit_2_00000000000000000000000000000002 = 10;
	const FVaRestJson Json = FVaRestJsonConvert::ToJson(Value);
	TestEqual(TEXT("uppercase key retains its own field"), Json[TEXT("Wire")].AsInt(), 7);
	TestEqual(TEXT("lowercase key retains its own field"), Json[TEXT("wire")].AsInt(), 10);
	auto EnumRules = Enums();
	TestTrue(TEXT("register enum names"), FVaRestJsonRules::RegisterEnum(A.Get(), StaticEnum<EVaRestJsonTestColor>(), EnumRules));
	EnumRules[0].Json = TEXT("RED-WIRE");
	TestTrue(TEXT("register enum case-only difference"), FVaRestJsonRules::RegisterEnum(B.Get(), StaticEnum<EVaRestJsonTestColor>(), EnumRules));
	TestFalse(TEXT("case-only enum declarations conflict"), FVaRestJsonRules::DefaultSnapshot()->Find(StaticEnum<EVaRestJsonTestColor>())->Error.IsEmpty());
	FVaRestJsonRules::UnregisterOwner(B.Get());
	Rules[0].Json = TEXT("colors"); // collides late with an unmapped reflected field, after a populated prefix
	TestTrue(TEXT("publish a rule whose collision depends on the actual field map"), FVaRestJsonRules::RegisterFields(A.Get(), Type, Rules));
	TArray<const FProperty*> Positional;
	FVaRestJsonError Error;
	TestFalse(TEXT("late field-map collision refuses positional output"), VaRest::Json::TryGetConvertedFieldsInternal(Type, Positional, {}, &Error));
	TestTrue(TEXT("late collision does not expose a field prefix"), Positional.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRulesPartialEnumTest, "VaRest.Json.Rules.PartialEnumNamespace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonRulesPartialEnumTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonRulesTest;
	FOwner Owner;
	FVaRestJsonEnumRule Red;
	Red.Enumerator = TEXT("Red");
	Red.Json = TEXT("Green");
	TArray<FVaRestJsonEnumRule> Rules{Red};
	FString Error;
	const uint64 Generation = FVaRestJsonRules::GetDefaultGeneration();
	TestFalse(TEXT("partial mapping cannot collide with an unmapped authored name"), FVaRestJsonRules::RegisterEnum(Owner.Get(), StaticEnum<EVaRestJsonTestColor>(), Rules, &Error));
	TestEqual(TEXT("ambiguous enum publishes nothing"), FVaRestJsonRules::GetDefaultGeneration(), Generation);
	Rules[0].Json = TEXT("green");
	TestTrue(TEXT("exact case-distinct mapping remains invertible"), FVaRestJsonRules::RegisterEnum(Owner.Get(), StaticEnum<EVaRestJsonTestColor>(), Rules));
	FVaRestJsonTestAll Value;
	Value.Color = EVaRestJsonTestColor::Red;
	Value.ByColor.Add(EVaRestJsonTestColor::Red, 1);
	Value.ByColor.Add(EVaRestJsonTestColor::Green, 2);
	const auto Json = FVaRestJsonConvert::ToJson(Value);
	TestEqualSensitive(TEXT("mapped scalar"), Json[TEXT("color")].AsString(), FString(TEXT("green")));
	FVaRestJsonConvertOptions Strict;
	Strict.bStrict = true;
	FVaRestJsonDecodeReport Report;
	FVaRestJsonTestAll Decoded;
	TestTrue(TEXT("scalar and enum map keys round trip"), FVaRestJsonConvert::JsonToStruct(Json, FVaRestJsonTestAll::StaticStruct(), &Decoded, Strict, &Report));
	TestEqual(TEXT("mapped scalar preserves its enumerator"), Decoded.Color, EVaRestJsonTestColor::Red);
	TestTrue(TEXT("distinct mapped and fallback map keys survive"), Decoded.ByColor.OrderIndependentCompareEqual(Value.ByColor));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRulesAliasTest, "VaRest.Json.Rules.EnumAliases",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonRulesAliasTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonRulesTest;
	FOwner Owner;
	TStrongObjectPtr<UEnum> Enum(NewObject<UEnum>());
	TArray<TPair<FName, int64>> Names;
	Names.Emplace(FName(*(Enum->GetName() + TEXT("::A"))), 0);
	Names.Emplace(FName(*(Enum->GetName() + TEXT("::Alias"))), 0);
	Names.Emplace(FName(*(Enum->GetName() + TEXT("::C"))), 1);
	if (!TestTrue(TEXT("real UEnum supports value aliases"), Enum->SetEnums(Names, UEnum::ECppForm::EnumClass, UEnum::EUnderlyingType::uint8, EEnumFlags::None, UEnum::EAddMaxKeyIfMissing::No)))
	{
		return false;
	}
	TStrongObjectPtr<UScriptStruct> PropertyOwner(NewObject<UScriptStruct>());
	TUniquePtr<FEnumProperty> Property = MakeUnique<FEnumProperty>(FFieldVariant(PropertyOwner.Get()), TEXT("Value"));
	Property->SetEnum(Enum.Get());
	Property->ArrayDim = 1;
	Property->AddCppProperty(new FByteProperty(FFieldVariant(Property.Get()), TEXT("Underlying")));
	FVaRestJsonEnumRule Rule;
	Rule.Enumerator = TEXT("C");
	Rule.Json = TEXT("Alias");
	const TArray<FVaRestJsonEnumRule> Rules{Rule};
	TestTrue(TEXT("mapping targets a distinct runtime value"), FVaRestJsonRules::RegisterEnum(Owner.Get(), Enum.Get(), Rules));
	const uint8 Original = 1;
	const auto Json = FVaRestJsonConvert::PropertyToJson(Property.Get(), &Original);
	TestEqualSensitive(TEXT("mapped scalar wire spelling"), Json.AsString(), FString(TEXT("Alias")));
	FVaRestJsonConvertOptions Strict;
	Strict.bStrict = true;
	FVaRestJsonDecodeReport Report;
	uint8 Decoded = 255;
	TestTrue(TEXT("mapped alias spelling decodes"), FVaRestJsonConvert::JsonToProperty(Json, Property.Get(), &Decoded, Strict, &Report));
	TestEqual(TEXT("an unmapped alias cannot steal the mapped value"), Decoded, uint8(1));
	FVaRestJsonRules::UnregisterOwner(Owner.Get());
	Decoded = 255;
	TestTrue(TEXT("unregistered code-first alias spelling still decodes"), FVaRestJsonConvert::JsonToProperty(Json, Property.Get(), &Decoded, Strict, &Report));
	TestEqual(TEXT("legacy alias retains its native value"), Decoded, uint8(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRulesNumericMapTest, "VaRest.Json.Rules.NumericEnumMapNamespace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonRulesNumericMapTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonRulesTest;
	FOwner Owner;
	FVaRestJsonEnumRule Rule;
	Rule.Enumerator = TEXT("Red");
	Rule.Json = TEXT("1");
	TArray<FVaRestJsonEnumRule> Rules{Rule};
	TestTrue(TEXT("mapping is valid in the ordinary named namespace"), FVaRestJsonRules::RegisterEnum(Owner.Get(), StaticEnum<EVaRestJsonTestColor>(), Rules));
	FVaRestJsonTestAll Value;
	Value.ByColor.Add(EVaRestJsonTestColor::Red, 1);
	Value.ByColor.Add(EVaRestJsonTestColor::Green, 2);
	FVaRestJsonConvertOptions Numeric;
	Numeric.bEnumsAsNumbers = true;
	const FProperty* Scalar = FindFProperty<FProperty>(FVaRestJsonTestAll::StaticStruct(), TEXT("Color"));
	const EVaRestJsonTestColor Red = EVaRestJsonTestColor::Red;
	const EVaRestJsonTestColor Green = EVaRestJsonTestColor::Green;
	const auto RedJson = FVaRestJsonConvert::PropertyToJson(Scalar, &Red, Numeric);
	const auto GreenJson = FVaRestJsonConvert::PropertyToJson(Scalar, &Green, Numeric);
	TestTrue(TEXT("mapped scalar string remains distinct from numeric scalar fallback"), RedJson.IsString() && GreenJson.IsNumber());
	TestEqualSensitive(TEXT("mapped scalar string"), RedJson.AsString(), FString(TEXT("1")));
	TestEqual(TEXT("numeric scalar fallback"), GreenJson.AsInt(), 1);
	TArray<uint8> Output{99};
	FVaRestJsonError Error;
	TestFalse(TEXT("mapped numeric string cannot alias an unmapped numeric map key"), FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestAll::StaticStruct(), &Value, Output, Numeric, EVaRestJsonFormat::Condensed, &Error));
	TestTrue(TEXT("refusal preserves the complete caller prefix"), Output == TArray<uint8>({99}));
	Value.ByColor.Remove(EVaRestJsonTestColor::Red);
	TestFalse(TEXT("a single fallback key cannot silently decode as the mapped value"), FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestAll::StaticStruct(), &Value, Output, Numeric, EVaRestJsonFormat::Condensed, &Error));
	Rules[0].Json = TEXT("red-wire");
	TestTrue(TEXT("distinct mapping and numeric fallback are allowed"), FVaRestJsonRules::RegisterEnum(Owner.Get(), StaticEnum<EVaRestJsonTestColor>(), Rules));
	Value.ByColor.Add(EVaRestJsonTestColor::Red, 1);
	const auto Json = FVaRestJsonConvert::ToJson(Value, Numeric);
	FVaRestJsonTestAll Decoded;
	FVaRestJsonDecodeReport Report;
	Numeric.bStrict = true;
	TestTrue(TEXT("distinct numeric-key mode round trips"), FVaRestJsonConvert::JsonToStruct(Json, FVaRestJsonTestAll::StaticStruct(), &Decoded, Numeric, &Report));
	TestTrue(TEXT("both native enum keys survive"), Decoded.ByColor.OrderIndependentCompareEqual(Value.ByColor));
	Rules[0].Json = TEXT("255");
	TestTrue(TEXT("undeclared cast values are not guessed during registration"), FVaRestJsonRules::RegisterEnum(Owner.Get(), StaticEnum<EVaRestJsonTestColor>(), Rules));
	Value.ByColor.Reset();
	Value.ByColor.Add(EVaRestJsonTestColor::Red, 1);
	Value.ByColor.Add(static_cast<EVaRestJsonTestColor>(255), 2);
	TestFalse(TEXT("actual emitted-key check also catches unknown/cast enum values"), FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestAll::StaticStruct(), &Value, Output, {}, EVaRestJsonFormat::Condensed, &Error));
	TestTrue(TEXT("cast collision also preserves the caller prefix"), Output == TArray<uint8>({99}));
	Value.ByColor.Remove(EVaRestJsonTestColor::Red);
	TestFalse(TEXT("a lone cast key cannot decode as an absent mapped enumerator"), FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestAll::StaticStruct(), &Value, Output, {}, EVaRestJsonFormat::Condensed, &Error));
	TestFalse(TEXT("numeric mode also refuses the lone cast reverse collision"), FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestAll::StaticStruct(), &Value, Output, Numeric, EVaRestJsonFormat::Pretty, &Error));
	TestTrue(TEXT("singleton/pretty failures preserve the complete prefix"), Output == TArray<uint8>({99}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRulesUnicodeNamespaceTest, "VaRest.Json.Rules.UnicodeWireNamespace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonRulesUnicodeNamespaceTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonRulesTest;
	FOwner Owner;
	FVaRestJsonEnumRule Red;
	Red.Enumerator = TEXT("Red");
	Red.Json = TEXT("stable-red");
	FVaRestJsonEnumRule Green;
	Green.Enumerator = TEXT("Green");
	Green.Json.AppendChar(TCHAR(0xFFFD));
	TArray<FVaRestJsonEnumRule> Rules{Red, Green};
	TestTrue(TEXT("well-formed replacement character is a valid wire spelling"), FVaRestJsonRules::RegisterEnum(Owner.Get(), StaticEnum<EVaRestJsonTestColor>(), Rules));
	const uint64 Generation = FVaRestJsonRules::GetGeneration(Owner.Get());
	const auto Before = FVaRestJsonRules::Snapshot(Owner.Get());
	FString Error;
	Rules[0].Json.Reset();
	Rules[0].Json.AppendChar(TCHAR(0xD800));
	TestFalse(TEXT("a lone high surrogate cannot be repaired into another mapping"), FVaRestJsonRules::RegisterEnum(Owner.Get(), StaticEnum<EVaRestJsonTestColor>(), Rules, &Error));
	TestFalse(TEXT("malformed rule has a diagnostic"), Error.IsEmpty());
	TestEqual(TEXT("refused publication leaves the owner generation"), FVaRestJsonRules::GetGeneration(Owner.Get()), Generation);
	TestTrue(TEXT("refused publication leaves the same immutable snapshot"), FVaRestJsonRules::Snapshot(Owner.Get()) == Before);
	Rules[0].Json.Reset();
	Rules[0].Json.AppendChar(TCHAR(0xDC00));
	TestFalse(TEXT("a lone low surrogate is also refused"), FVaRestJsonRules::RegisterEnum(Owner.Get(), StaticEnum<EVaRestJsonTestColor>(), Rules, &Error));
	FVaRestJsonTestAll Value;
	Value.Color = EVaRestJsonTestColor::Red;
	Value.ByColor.Add(EVaRestJsonTestColor::Red, 9);
	const auto StillValid = FVaRestJsonConvert::ToJson(Value);
	TestEqualSensitive(TEXT("old scalar mapping survives both invalid replacements"), StillValid[TEXT("color")].AsString(), FString(TEXT("stable-red")));

	Rules[0].Json.Reset();
	Rules[0].Json.AppendChar(TCHAR(0xD83D));
	Rules[0].Json.AppendChar(TCHAR(0xDD34));
	TestTrue(TEXT("a genuine surrogate pair stays distinct from U+FFFD"), FVaRestJsonRules::RegisterEnum(Owner.Get(), StaticEnum<EVaRestJsonTestColor>(), Rules, &Error));
	const auto Json = FVaRestJsonConvert::ToJson(Value);
	TestEqualSensitive(TEXT("well-formed scalar is not rewritten"), Json[TEXT("color")].AsString(), Rules[0].Json);
	FVaRestJsonTestAll Decoded;
	FVaRestJsonConvertOptions Strict;
	Strict.bStrict = true;
	TestTrue(TEXT("well-formed scalar and singleton map strictly decode"), FVaRestJsonConvert::JsonToStruct(Json, FVaRestJsonTestAll::StaticStruct(), &Decoded, Strict));
	TestEqual(TEXT("scalar native enum survives"), Decoded.Color, Value.Color);
	TestTrue(TEXT("singleton native map key survives"), Decoded.ByColor.OrderIndependentCompareEqual(Value.ByColor));

	FVaRestJsonFieldRule Field;
	Field.Field = FGuid::NewGuid();
	Field.Json.AppendChar(TCHAR(0xD800));
	TestFalse(TEXT("field keys obey the same stable Unicode publication rule"), FVaRestJsonRules::RegisterFields(Owner.Get(), FVaRestJsonTestRules::StaticStruct(), TArray<FVaRestJsonFieldRule>{Field}, &Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRulesPublicationTest, "VaRest.Json.Rules.AtomicPublication",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonRulesPublicationTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonRulesTest;
	FOwner Owner;
	FVaRestJsonTestRules Value;
	Value.Color_8_00000000000000000000000000000008 = EVaRestJsonTestColor::Red;
	Value.Colors = {EVaRestJsonTestColor::Red};
	const FString Baseline = FVaRestJsonConvert::ToJson(Value).ToString();
	TestTrue(TEXT("register fields"), FVaRestJsonRules::RegisterFields(Owner.Get(), FVaRestJsonTestRules::StaticStruct(), Fields()));
	TestTrue(TEXT("register enum"), FVaRestJsonRules::RegisterEnum(Owner.Get(), StaticEnum<EVaRestJsonTestColor>(), Enums()));
	FVaRestJsonConvertOptions Options;
	bool bChanged = false;
	Options.BytesWriter = [&](TConstArrayView<uint8>, FVaRestJson&) {
		TArray<FVaRestJsonEnumRule> Replacement = Enums();
		Replacement[0].Json = TEXT("new-red");
		bChanged = FVaRestJsonRules::RegisterEnum(Owner.Get(), StaticEnum<EVaRestJsonTestColor>(), Replacement);
		return false;
	};
	const FVaRestJson During = FVaRestJsonConvert::ToJson(Value, Options);
	TestTrue(TEXT("rule update during conversion is admitted"), bChanged);
	TestEqualSensitive(TEXT("current conversion retains its initial rules"), During[TEXT("color")].AsString(), FString(TEXT("red-wire")));
	TestEqualSensitive(TEXT("nested array also retains that snapshot"), During[TEXT("colors")][0].AsString(), FString(TEXT("red-wire")));
	TestEqualSensitive(TEXT("next conversion sees the new generation"), FVaRestJsonConvert::ToJson(Value)[TEXT("color")].AsString(), FString(TEXT("new-red")));
	const uint64 BeforeInvalid = FVaRestJsonRules::GetDefaultGeneration();
	TArray<FVaRestJsonFieldRule> Invalid = Fields();
	Invalid[1].Field = Invalid[0].Field;
	FString Error;
	TestFalse(TEXT("duplicate field identities refuse the entire update"), FVaRestJsonRules::RegisterFields(Owner.Get(), FVaRestJsonTestRules::StaticStruct(), Invalid, &Error));
	TestEqual(TEXT("invalid batch publishes no generation"), FVaRestJsonRules::GetDefaultGeneration(), BeforeInvalid);
	TestEqualSensitive(TEXT("invalid batch keeps prior rules"), FVaRestJsonConvert::ToJson(Value)[TEXT("color")].AsString(), FString(TEXT("new-red")));
	FVaRestJsonRules::UnregisterOwner(Owner.Get());
	TestEqualSensitive(TEXT("unregister restores ordinary code-first conversion"), FVaRestJsonConvert::ToJson(Value).ToString(), Baseline);
	return true;
}

#endif
