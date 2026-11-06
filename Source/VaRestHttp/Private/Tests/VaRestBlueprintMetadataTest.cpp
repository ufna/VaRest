// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"
#include "VaRestHttpBlueprintTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestBlueprintNumericIdsTest, "VaRest.Http.Blueprint.LosslessIds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestBlueprintNumericIdsTest::RunTest(const FString& Parameters)
{
	const uint64 Values[] = {0, 1, static_cast<uint64>(MAX_int64) + 1, MAX_uint64};
	for (uint64 Expected : Values)
	{
		uint64 Actual = 17;
		const FString Text = VaRest::Http::UnsignedIdText(Expected);
		TestTrue(TEXT("canonical ID parses"), VaRest::Http::TryParseUnsignedId(Text, Actual));
		TestEqual(TEXT("ID preserves all64 unsigned bits"), Actual, Expected);
	}
	TestEqualSensitive(TEXT("maximum unsigned ID is decimal, not a negative or floating value"), VaRest::Http::UnsignedIdText(MAX_uint64), FString(TEXT("18446744073709551615")));
	const TCHAR* Invalid[] = {TEXT(""), TEXT("00"), TEXT("01"), TEXT("+1"), TEXT("-1"), TEXT(" 1"), TEXT("1 "), TEXT("1.0"), TEXT("1e1"), TEXT("18446744073709551616"), TEXT("١")};
	for (const TCHAR* Text : Invalid)
	{
		uint64 Actual = 23;
		TestFalse(TEXT("noncanonical or overflowing ID is refused"), VaRest::Http::TryParseUnsignedId(Text, Actual));
		TestEqual(TEXT("refused ID preserves caller output"), Actual, uint64(23));
	}
	const TCHAR IdUnits[] = {TEXT('1'), TCHAR(0), TEXT('2')};
	const FString EmbeddedNull = FString::ConstructFromPtrSize(IdUnits, UE_ARRAY_COUNT(IdUnits));
	if (!TestTrue(TEXT("ID fixture contains the counted interior NUL"), EmbeddedNull.Len() == 3 && EmbeddedNull[1] == 0))
		return false;
	uint64 Actual = 23;
	TestFalse(TEXT("embedded null cannot truncate an ID"), VaRest::Http::TryParseUnsignedId(EmbeddedNull, Actual));
	TestEqual(TEXT("embedded-null refusal preserves caller output"), Actual, uint64(23));
	uint32 Generation = 0;
	TestTrue(TEXT("maximum generation parses"), VaRest::Http::TryParseGeneration(TEXT("4294967295"), Generation));
	TestEqual(TEXT("maximum generation preserved"), Generation, MAX_uint32);
	TestFalse(TEXT("generation cannot overflow32 bits"), VaRest::Http::TryParseGeneration(TEXT("4294967296"), Generation));
	TestEqual(TEXT("overflow preserves generation"), Generation, MAX_uint32);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestBlueprintResponseDataTest, "VaRest.Http.Blueprint.ResponseCopy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestBlueprintResponseDataTest::RunTest(const FString& Parameters)
{
	FVaRestQueuedResult Native;
	Native.QueueId = TEXT("opaque-queue-id");
	Native.Tag = TEXT("original-tag");
	Native.bSuccess = false;
	Native.Response.RequestId = MAX_uint64;
	Native.Response.Status = 503;
	Native.Response.Headers.Add(TEXT("Set-Cookie"), TEXT("a=1"));
	Native.Response.Headers.Add(TEXT("Set-Cookie"), TEXT("b=2"));
	Native.Response.Body = {0, 1, 255};
	Native.Response.BytesDown = 3;
	Native.Error.Kind = EVaRestErrorKind::Http;
	const FVaRestQueuedResultData Data = FVaRestQueuedResultData::FromNative(Native);
	Native.Response.Body[0] = 42;
	TestEqualSensitive(TEXT("queue identity stays opaque"), Data.QueueId, FString(TEXT("opaque-queue-id")));
	TestEqualSensitive(TEXT("response ID is lossless"), Data.Response.RequestId, FString(TEXT("18446744073709551615")));
	TestEqual(TEXT("all raw bytes retained"), Data.Response.Body.Num(), 3);
	TestTrue(TEXT("response body is an owned exact copy"), Data.Response.Body == TArray<uint8>({0, 1, 255}));
	TestEqual(TEXT("repeated headers remain separate"), Data.Response.Headers.Num(), 2);
	if (Data.Response.Headers.Num() == 2)
	{
		TestEqualSensitive(TEXT("first header"), Data.Response.Headers[0].Value, FString(TEXT("a=1")));
		TestEqualSensitive(TEXT("second header"), Data.Response.Headers[1].Value, FString(TEXT("b=2")));
	}
	TestEqual(TEXT("error kind is retained"), Data.Error.Kind, EVaRestErrorKind::Http);
	TestNotNull(TEXT("reflected ID uses string storage"), FindFProperty<FStrProperty>(FVaRestResponseData::StaticStruct(), TEXT("RequestId")));
	TestNotNull(TEXT("native signed byte count remains int64"), FindFProperty<FInt64Property>(FVaRestResponseData::StaticStruct(), TEXT("BytesDown")));
	FVaRestSessionData Session;
	Session.Generation = MAX_uint32;
	Session.CredentialGeneration = MAX_uint32;
	const auto View = FVaRestSessionDataView::FromNative(Session);
	TestEqual(TEXT("session identity is not replaced with a new GUID"), View.IdentityId, Session.IdentityId);
	TestEqualSensitive(TEXT("session generation is lossless"), View.Generation, FString(TEXT("4294967295")));
	TestEqualSensitive(TEXT("credential generation is lossless"), View.CredentialGeneration, FString(TEXT("4294967295")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestBlueprintMultipartTest, "VaRest.Http.Blueprint.MultipartSources",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestBlueprintMultipartTest::RunTest(const FString& Parameters)
{
	FVaRestMultipartData Data;
	FVaRestMultipartPartData Field;
	Field.bField = true;
	Field.Name = TEXT("field");
	Field.FieldValue = TEXT("a&b");
	Data.Parts.Add(Field);
	FVaRestMultipartPartData Bytes;
	Bytes.Name = TEXT("binary");
	Bytes.FileName = TEXT("sample.bin");
	Bytes.Bytes = {0, 1, 255};
	Data.Parts.Add(Bytes);
	auto Converted = Data.ToNative();
	if (TestTrue(TEXT("field and in-memory binary sources convert"), Converted.HasValue()))
	{
		const auto& Parts = Converted.GetValue().GetParts();
		TestEqual(TEXT("part order/count is preserved"), Parts.Num(), 2);
		if (Parts.Num() == 2)
		{
			TestTrue(TEXT("field uses native field representation"), Parts[0].bField);
			TestTrue(TEXT("binary payload retains exact bytes"), Parts[1].Bytes == Bytes.Bytes);
		}
	}
	Data.Parts[0].Bytes = {1};
	TestFalse(TEXT("field cannot silently discard a second bytes source"), Data.ToNative().HasValue());
	Data.Parts[0].Bytes.Reset();
	Data.Parts[1].FieldValue = TEXT("hidden");
	TestFalse(TEXT("binary part cannot silently discard a field value"), Data.ToNative().HasValue());
	return true;
}

#endif
