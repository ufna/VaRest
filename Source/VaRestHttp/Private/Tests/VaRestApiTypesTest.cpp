// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "VaRestApiTypes.h"
#include "Misc/AutomationTest.h"
#include "VaRestBody.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestApiTypeDescriptorsTest, "VaRest.Http.Api.TypeDescriptors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestApiTypeDescriptorsTest::RunTest(const FString& Parameters)
{
	for (uint8 Kind = 0; Kind <= static_cast<uint8>(EVaRestTypeKind::DateTime); ++Kind)
	{
		for (uint8 Container = 0; Container <= static_cast<uint8>(EVaRestTypeContainer::Map); ++Container)
		{
			FVaRestTypeRef Type;
			Type.Kind = static_cast<EVaRestTypeKind>(Kind);
			Type.Container = static_cast<EVaRestTypeContainer>(Container);
			if (Type.Kind == EVaRestTypeKind::Struct)
			{
				Type.Type = FVaRestJson::StaticStruct();
			}
			else if (Type.Kind == EVaRestTypeKind::Enum)
			{
				Type.Type = StaticEnum<EVaRestPriority>();
			}
			FVaRestApiValidationContext Context;
			const bool bValid = Type.Validate(Context, TEXT("ResponseType"));
			const bool bExpected = Container == 0 || (Type.Kind != EVaRestTypeKind::None && Type.Kind != EVaRestTypeKind::Bytes);
			TestEqual(FString::Printf(TEXT("type%d/container%d"), Kind, Container), bValid, bExpected);
			TestEqual(TEXT("validation result agrees with emitted errors"), Context.HasErrors(), !bValid);
		}
	}
	FVaRestTypeRef Type;
	Type.Kind = EVaRestTypeKind::Struct;
	FVaRestApiValidationContext Missing;
	TestFalse(TEXT("a struct descriptor requires its actual reflection type"), Type.Validate(Missing, TEXT("Type")));
	Type.Type = StaticEnum<EVaRestPriority>();
	FVaRestApiValidationContext Wrong;
	TestFalse(TEXT("an enum object cannot stand in for a struct"), Type.Validate(Wrong, TEXT("Type")));
	Type.Kind = EVaRestTypeKind::DateTime;
	FVaRestApiValidationContext Date;
	TestFalse(TEXT("date-time has no arbitrary type object"), Type.Validate(Date, TEXT("Type")));
	Type.Type = nullptr;
	Type.Kind = static_cast<EVaRestTypeKind>(255);
	FVaRestApiValidationContext Corrupt;
	TestFalse(TEXT("unknown serialized kind is refused"), Type.Validate(Corrupt, TEXT("Type")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestApiParameterDescriptorsTest, "VaRest.Http.Api.ParameterDescriptors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestApiParameterDescriptorsTest::RunTest(const FString& Parameters)
{
	FVaRestEndpointParam Param;
	Param.Name = TEXT("sort");
	Param.PinName = TEXT("SortPin");
	Param.Type.Kind = EVaRestTypeKind::String;
	FVaRestApiValidationContext Valid;
	Param.Validate(Valid, TEXT("Params[0]"));
	TestFalse(TEXT("wire name and distinct pin identity are valid"), Valid.HasErrors());
	Param.In = EVaRestEndpointParamLocation::Path;
	Param.Type.Container = EVaRestTypeContainer::Array;
	FVaRestApiValidationContext Repeat;
	Param.Validate(Repeat, TEXT("Params[0]"));
	TestTrue(TEXT("path arrays cannot silently use repeated-query encoding"), Repeat.HasErrors());
	Param.Style = EVaRestQueryStyle::Csv;
	FVaRestApiValidationContext Csv;
	Param.Validate(Csv, TEXT("Params[0]"));
	TestFalse(TEXT("declared path array uses comma encoding"), Csv.HasErrors());
	Param.Type.Kind = EVaRestTypeKind::Json;
	FVaRestApiValidationContext Json;
	Param.Validate(Json, TEXT("Params[0]"));
	TestTrue(TEXT("an arbitrary JSON body is not a parameter descriptor"), Json.HasErrors());
	Param.Type.Kind = EVaRestTypeKind::String;
	const TCHAR NameUnits[] = {TEXT('i'), TEXT('d'), TCHAR(0), TEXT('s')};
	Param.Name = FString::ConstructFromPtrSize(NameUnits, UE_ARRAY_COUNT(NameUnits));
	if (!TestTrue(TEXT("parameter fixture contains the counted interior NUL"), Param.Name.Len() == 4 && Param.Name[2] == 0))
		return false;
	FVaRestApiValidationContext Null;
	Param.Validate(Null, TEXT("Params[0]"));
	TestTrue(TEXT("embedded null cannot truncate a parameter wire name"), Null.HasErrors());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestApiBodyAndHeadersTest, "VaRest.Http.Api.BodyKindsAndOrderedHeaders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestApiBodyAndHeadersTest::RunTest(const FString& Parameters)
{
	const EVaRestEndpointBodyKind AssetKinds[] = {EVaRestEndpointBodyKind::Json, EVaRestEndpointBodyKind::Form,
		EVaRestEndpointBodyKind::Multipart, EVaRestEndpointBodyKind::Binary, EVaRestEndpointBodyKind::Text};
	const EVaRestBodyKind NativeKinds[] = {EVaRestBodyKind::Json, EVaRestBodyKind::Form,
		EVaRestBodyKind::Multipart, EVaRestBodyKind::Binary, EVaRestBodyKind::Text};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(AssetKinds); ++Index)
	{
		EVaRestBodyKind Native = EVaRestBodyKind::None;
		TestTrue(TEXT("every authored kind has a native mapping"), VaRest::Http::TryGetNativeBodyKind(AssetKinds[Index], Native));
		TestEqual(TEXT("symbolic mapping preserves existing native enum values"), Native, NativeKinds[Index]);
	}
	EVaRestBodyKind Unchanged = EVaRestBodyKind::Stream;
	TestFalse(TEXT("unknown authored kind is refused"), VaRest::Http::TryGetNativeBodyKind(static_cast<EVaRestEndpointBodyKind>(255), Unchanged));
	TestEqual(TEXT("invalid selection does not manufacture a native kind"), Unchanged, EVaRestBodyKind::Stream);
	FVaRestHeaders Native;
	Native.Add(TEXT("Set-Cookie"), TEXT("a=1"));
	Native.Add(TEXT("X-Example"), TEXT("middle"));
	Native.Add(TEXT("Set-Cookie"), TEXT("b=2"));
	const auto Rows = VaRest::Http::ToHeaderData(Native);
	const auto RoundTrip = VaRest::Http::ToNativeHeaders(Rows);
	TestEqual(TEXT("all repeated rows survive reflection conversion"), RoundTrip.Num(), Native.Num());
	for (int32 Index = 0; Index < Native.Num() && Index < RoundTrip.Num(); ++Index)
	{
		TestEqualSensitive(TEXT("header spelling is preserved"), RoundTrip.GetPairs()[Index].Key, Native.GetPairs()[Index].Key);
		TestEqualSensitive(TEXT("header value is preserved"), RoundTrip.GetPairs()[Index].Value, Native.GetPairs()[Index].Value);
	}
	const FVaRestApiEndpoint A;
	const FVaRestApiEndpoint B;
	TestTrue(TEXT("new endpoints get distinct stable identities"), A.Id.IsValid() && B.Id.IsValid() && A.Id != B.Id);
	const FVaRestApiEndpoint Copy = A;
	TestTrue(TEXT("copy/rename preserves endpoint identity"), Copy.Id == A.Id);
	TestEqual(TEXT("new endpoint response defaults to JSON"), A.ResponseType.Kind, EVaRestTypeKind::Json);
	TestEqual(TEXT("new endpoint error has no typed payload"), A.ErrorType.Kind, EVaRestTypeKind::None);
	return true;
}

#endif
