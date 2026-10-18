// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "VaRestJson.h"
#include "VaRestJsonNode.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonBridgeTest, "VaRest.Json.Bridge.RoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonBridgeTest::RunTest(const FString& Parameters)
{
	const FVaRestJson J = FVaRestJson::Parse(FStringView(TEXT(
		"{\"s\":\"é\",\"i\":42,\"d\":1.5,\"b\":true,\"n\":null,\"a\":[1,{\"k\":\"v\"}],\"big\":9007199254740993,\"u\":18446744073709551615}"))).StealValue();

	const TSharedPtr<FJsonObject> O = J.ToJsonObject();
	TestTrue(TEXT("object"), O.IsValid());
	TestEqualSensitive(TEXT("string"), O->GetStringField(TEXT("s")), FString(TEXT("é")));
	TestEqual(TEXT("int as number"), O->GetNumberField(TEXT("i")), 42.0);
	TestEqualSensitive(TEXT("nested"), O->GetArrayField(TEXT("a"))[1]->AsObject()->GetStringField(TEXT("k")), FString(TEXT("v")));

	// 2^53 + 1 does not fit a double: it must travel as a number string
	int64 Big = 0;
	const TSharedPtr<FJsonValue> BigField = O->TryGetField(TEXT("big"));
	TestTrue(TEXT("big int exact"), BigField.IsValid() && BigField->TryGetNumber(Big) && Big == 9007199254740993ll);

	const FVaRestJson Back = FVaRestJson::FromJsonObject(O);
	TestTrue(TEXT("round trip equals"), Back.Equals(J));
	TestEqual(TEXT("big int back"), Back[TEXT("big")].AsInt64(), 9007199254740993ll);
	TestEqual(TEXT("uint64 back"), Back[TEXT("u")].AsUInt64(), MAX_uint64);
	TestEqualSensitive(TEXT("engine double 42 comes back as integer"), Back[TEXT("i")].ToString(), FString(TEXT("42")));

	// An engine-parsed tree converts too
	TSharedPtr<FJsonValue> Engine;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("[1,\"x\",{\"z\":false}]")), Engine);
	const FVaRestJson FromEngine = FVaRestJson::FromJsonValue(Engine);
	TestEqualSensitive(TEXT("engine tree"), FromEngine.ToString(), FString(TEXT("[1,\"x\",{\"z\":false}]")));

	// Duplicates collapse to the last occurrence on the way to FJsonObject
	const FVaRestJson Dup = FVaRestJson::Parse(FStringView(TEXT("{\"a\":1,\"a\":2}"))).StealValue();
	TestEqual(TEXT("dup -> last"), Dup.ToJsonObject()->GetNumberField(TEXT("a")), 2.0);

	TestFalse(TEXT("ToJsonObject on array is null"), FromEngine.ToJsonObject().IsValid());
	TestFalse(TEXT("null shared ptr -> invalid"), FVaRestJson::FromJsonValue(nullptr).IsValid());
	return true;
}

namespace
{
	/** An engine value whose Type is None: the bridge must treat it as null */
	class FNoneTypedJsonValue : public FJsonValue
	{
	public:
		FNoneTypedJsonValue()
		{
			Type = EJson::None;
		}

		virtual FString GetType() const override
		{
			return TEXT("None");
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonBridgeNumbersTest, "VaRest.Json.Bridge.Numbers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonBridgeNumbersTest::RunTest(const FString& Parameters)
{
	// Plain FJsonValueNumber: its TryGetString is SanitizeFloat (6 fractional digits), so its text is not authoritative
	auto Plain = [](double V) { return FVaRestJson::FromJsonValue(MakeShared<FJsonValueNumber>(V)); };
	auto Text = [](const TCHAR* S) { return FVaRestJson::FromJsonValue(MakeShared<FJsonValueNumberString>(FString(S))); };

	const FVaRestJson A = Plain(0.9999999);
	TestTrue(TEXT("0.9999999 stays a double"), A.GetNodeInternal() && A.GetNodeInternal()->NumKind == VaRest::Json::ENumKind::Double);
	TestEqual(TEXT("0.9999999 value"), A.AsDouble(), 0.9999999);

	const FVaRestJson B = Plain(1e-7);
	TestTrue(TEXT("1e-7 stays a double"), B.GetNodeInternal() && B.GetNodeInternal()->NumKind == VaRest::Json::ENumKind::Double);
	TestEqual(TEXT("1e-7 value"), B.AsDouble(), 1e-7);

	const FVaRestJson C = Plain(41.9999996);
	TestTrue(TEXT("41.9999996 stays a double"), C.GetNodeInternal() && C.GetNodeInternal()->NumKind == VaRest::Json::ENumKind::Double);
	TestEqual(TEXT("41.9999996 value"), C.AsDouble(), 41.9999996);

	const FVaRestJson D = Plain(1e18);
	TestEqual(TEXT("1e18 exact integer"), D.AsInt64(), 1000000000000000000ll);

	const FVaRestJson E = Plain(1e300);
	TestTrue(TEXT("1e300 stays a double"), E.GetNodeInternal() && E.GetNodeInternal()->NumKind == VaRest::Json::ENumKind::Double);
	TestEqual(TEXT("1e300 value"), E.AsDouble(), 1e300);

	TestEqualSensitive(TEXT("integral double 5.0 writes as 5"), Plain(5.0).ToString(), FString(TEXT("5")));

	// FJsonValueNumberString: exact digits
	TestEqual(TEXT("2^53+1 exact"), Text(TEXT("9007199254740993")).AsInt64(), 9007199254740993ll);
	TestEqual(TEXT("int64 min exact"), Text(TEXT("-9223372036854775808")).AsInt64(), MIN_int64);
	TestEqual(TEXT("uint64 max exact"), Text(TEXT("18446744073709551615")).AsUInt64(), MAX_uint64);

	const FVaRestJson Overflow = Text(TEXT("99999999999999999999"));
	TestTrue(TEXT("int overflow becomes a double"), Overflow.IsNumber() && Overflow.GetNodeInternal()->NumKind == VaRest::Json::ENumKind::Double);
	TestEqual(TEXT("int overflow value"), Overflow.AsDouble(), 1e20);

	TestTrue(TEXT("malformed number string is null"), Text(TEXT("abc")).IsNull());

	// Nulls inside containers
	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Add(nullptr);
	Items.Add(MakeShared<FJsonValueNumber>(1.0));
	const FVaRestJson Arr = FVaRestJson::FromJsonValue(MakeShared<FJsonValueArray>(Items));
	TestEqual(TEXT("array keeps count"), Arr.Num(), 2);
	TestTrue(TEXT("null element -> null"), Arr.At(0).IsNull());

	const TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetField(FStringView(TEXT("x")), MakeShared<FNoneTypedJsonValue>());
	const FVaRestJson Nested = FVaRestJson::FromJsonObject(Obj);
	TestTrue(TEXT("None-typed member -> null"), Nested.Get(TEXT("x")).IsNull());

	TestFalse(TEXT("ToJsonObject on a number is null"), Plain(1.0).ToJsonObject().IsValid());
	return true;
}

#endif
