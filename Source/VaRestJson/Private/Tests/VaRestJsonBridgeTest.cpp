// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "VaRestJson.h"

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

#endif
