// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonConvertTestTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonEncodeBudgetTest, "VaRest.Json.Budget.Encode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonEncodeBudgetTest::RunTest(const FString& Parameters)
{
	FVaRestJsonTestTree Data;
	for (int32 Index = 0; Index < 32; ++Index)
		Data.Children.Add(FVaRestJson::Null());
	FVaRestJsonConvertOptions Options;
	Options.MaxValues = 8;
	TArray<uint8> Output{99, 100};
	const TArray<uint8> Prefix = Output;
	FVaRestJsonError Error;
	TestFalse(TEXT("nested JSON shares the complete struct value account"),
		FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestTree::StaticStruct(), &Data, Output, Options, EVaRestJsonFormat::Condensed, &Error));
	TestTrue(TEXT("logical value refusal restores the complete caller prefix"), Output == Prefix);
	TestTrue(TEXT("value refusal is explicit"), Error.Message.Contains(TEXT("value/key")));
	Options.MaxValues = 0;
	Options.MaxWorkUnits = 8;
	TestFalse(TEXT("nested JSON cannot bypass conversion work"),
		FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestTree::StaticStruct(), &Data, Output, Options, EVaRestJsonFormat::Pretty, &Error));
	TestTrue(TEXT("work refusal restores the complete caller prefix"), Output == Prefix);
	Options.MaxWorkUnits = 0;
	Options.MaxOutputBytes = 1;
	TestFalse(TEXT("logical bytes refuse before sink growth"),
		FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestTree::StaticStruct(), &Data, Output, Options, EVaRestJsonFormat::Condensed, &Error));
	TestTrue(TEXT("byte refusal preserves the caller prefix"), Output == Prefix);
	Options.MaxOutputBytes = 0;
	TestTrue(TEXT("all-zero options retain the ordinary serialized path"),
		FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestTree::StaticStruct(), &Data, Output, Options, EVaRestJsonFormat::Condensed, &Error));
	TestTrue(TEXT("successful output retains its original prefix"), Output.Num() > 2 && Output[0] == 99 && Output[1] == 100);
	FVaRestJsonTestBlob Blob;
	Blob.Data.Init(1, 4096);
	Options.bBytesAsBase64 = true;
	Options.MaxOutputBytes = 4096;
	Output = Prefix;
	TestFalse(TEXT("base64 encode checks its temporary before construction"),
		FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestBlob::StaticStruct(), &Blob, Output, Options, EVaRestJsonFormat::Condensed, &Error));
	TestTrue(TEXT("base64 encode refuses with the real byte diagnostic and intact prefix"),
		Error.Message.Contains(TEXT("output byte")) && Output == Prefix);
	Options.MaxOutputBytes = 0;
	Options.MaxWorkUnits = 32;
	TestFalse(TEXT("base64 encode cannot bypass work through a single string event"),
		FVaRestJsonConvert::StructToUtf8(FVaRestJsonTestBlob::StaticStruct(), &Blob, Output, Options, EVaRestJsonFormat::Condensed, &Error));
	TestTrue(TEXT("base64 work refusal preserves the prefix"), Output == Prefix);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDecodeBudgetTest, "VaRest.Json.Budget.Decode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDecodeBudgetTest::RunTest(const FString& Parameters)
{
	const FArrayProperty* Numbers = FindFProperty<FArrayProperty>(FVaRestJsonTestAll::StaticStruct(), TEXT("Numbers"));
	const FArrayProperty* Bytes = FindFProperty<FArrayProperty>(FVaRestJsonTestBlob::StaticStruct(), TEXT("Data"));
	const FStructProperty* Extra = FindFProperty<FStructProperty>(FVaRestJsonTestAll::StaticStruct(), TEXT("Extra"));
	if (!TestTrue(TEXT("real reflected array and JSON fixture properties exist"), Numbers && Bytes && Extra))
		return false;
	FVaRestJson Input = FVaRestJson::Array();
	for (int32 Index = 0; Index < 32; ++Index)
		Input.Add(0);
	TArray<int32> Destination{7, 8};
	const TArray<int32> Original = Destination;
	FVaRestJsonConvertOptions Options;
	Options.MaxValues = 8;
	FVaRestJsonDecodeReport Report;
	TestFalse(TEXT("lenient mode cannot waive a container initialization limit"),
		FVaRestJsonConvert::JsonToProperty(Input, Numbers, &Destination, Options, &Report));
	TestTrue(TEXT("container refusal precedes clearing or initializing its elements"), Destination == Original);
	TestTrue(TEXT("logical refusal is an error, not a warning"), !Report.Errors.IsEmpty());
	Options.MaxValues = 0;
	Options.MaxOutputBytes = 1;
	Report = {};
	TestFalse(TEXT("typed storage has a pre-growth byte check"),
		FVaRestJsonConvert::JsonToProperty(Input, Numbers, &Destination, Options, &Report));
	TestTrue(TEXT("byte refusal precedes clearing the destination"), Destination == Original);
	// Admit the header, but leave one byte less than the 32 int slots. No value/work limit can mask this gate.
	Options.MaxOutputBytes = Numbers->GetElementSize() + 32 * sizeof(int32) - 1;
	Report = {};
	TestFalse(TEXT("element-storage bytes are checked before array growth"),
		FVaRestJsonConvert::JsonToProperty(Input, Numbers, &Destination, Options, &Report));
	TestTrue(TEXT("element-storage refusal preserves the complete sentinel"), Destination == Original);
	TestTrue(TEXT("the element-storage gate reports its byte failure"), !Report.Errors.IsEmpty() && Report.Errors[0].Message.Contains(TEXT("output byte")));
	Options.MaxOutputBytes = 0;
	Report = {};
	TestTrue(TEXT("all-zero options retain ordinary array replacement"),
		FVaRestJsonConvert::JsonToProperty(Input, Numbers, &Destination, Options, &Report));
	TestEqual(TEXT("all-zero decode materializes every element"), Destination.Num(), 32);

	const FString Encoded = FString::ChrN(4096, TEXT('A'));
	const FVaRestJson Base64 = FVaRestJson::From(Encoded);
	TArray<uint8> Decoded{42};
	// Permit the counted TCHAR conversion/destination allowance, but refuse the two base64 byte buffers.
	Options.MaxOutputBytes = (int64(Encoded.Len()) + 1) * sizeof(TCHAR) * 2 + Bytes->GetElementSize() + 100;
	Report = {};
	TestFalse(TEXT("base64 buffer size is charged before decoding"),
		FVaRestJsonConvert::JsonToProperty(Base64, Bytes, &Decoded, Options, &Report));
	TestTrue(TEXT("base64 refusal leaves the field intact"), Decoded == TArray<uint8>{42});
	TestTrue(TEXT("base64 refusal records the logical byte failure"), !Report.Errors.IsEmpty() && Report.Errors[0].Message.Contains(TEXT("output byte")));

	FVaRestJson JsonDestination = FVaRestJson::Object();
	JsonDestination.Set(TEXT("keep"), 7);
	const FVaRestJson PriorNode = JsonDestination;
	Options.MaxOutputBytes = 0;
	Options.MaxValues = 4;
	Report = {};
	TestFalse(TEXT("deep JSON copy shares the account"),
		FVaRestJsonConvert::JsonToProperty(Input, Extra, &JsonDestination, Options, &Report));
	TestTrue(TEXT("refused JSON preflight does not replace or share a partial copy"), JsonDestination.IsSameNode(PriorNode));
	FVaRestJsonTestInner Inner;
	Inner.Name = TEXT("unchanged");
	Options = {};
	Options.MaxWorkUnits = 1;
	Report = {};
	const uint8 Wire[] = {'{', '"', 'n', 'a', 'm', 'e', '"', ':', '"', 'x', '"', '}'};
	TestFalse(TEXT("UTF8 converter propagates finite parsing controls before DOM construction"),
		FVaRestJsonConvert::Utf8ToStruct(TConstArrayView<uint8>(Wire), FVaRestJsonTestInner::StaticStruct(), &Inner, Options, &Report));
	TestEqual(TEXT("parse refusal leaves typed destination intact"), Inner.Name, FString(TEXT("unchanged")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonNativeDefaultBudgetTest, "VaRest.Json.Budget.NativeDefaultExclusion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonNativeDefaultBudgetTest::RunTest(const FString& Parameters)
{
	FVaRestJsonTestLargeDefault Large;
	FVaRestJsonConvertOptions Options;
	Options.MaxOutputBytes = 1024;
	FVaRestJsonDecodeReport Report;
	TestTrue(TEXT("absent text keeps the already initialized native default"),
		FVaRestJsonConvert::JsonToStruct(FVaRestJson::Object(), FVaRestJsonTestLargeDefault::StaticStruct(), &Large, Options, &Report));
	TestTrue(TEXT("logical budget is explicitly not a heap cap on native defaults"), Large.Text.GetAllocatedSize() > SIZE_T(Options.MaxOutputBytes));
	const FArrayProperty* Items = FindFProperty<FArrayProperty>(FVaRestJsonTestDefaultArray::StaticStruct(), TEXT("Items"));
	if (!TestTrue(TEXT("native-default array property exists"), Items != nullptr))
		return false;
	FVaRestJson Source = FVaRestJson::Array();
	for (int32 Index = 0; Index < 4; ++Index)
		Source.Add(FVaRestJson::Object());
	TArray<FVaRestJsonTestLargeDefault> Destination;
	TestTrue(TEXT("controlled fixed storage can admit excluded nested native defaults"),
		FVaRestJsonConvert::JsonToProperty(Source, Items, &Destination, Options, &Report));
	if (!TestEqual(TEXT("all default instances are initialized"), Destination.Num(), 4))
		return false;
	for (const auto& Item : Destination)
		TestEqual(TEXT("absent field retains its real native allocation"), Item.Text.Len(), 1024 * 1024);
	Options.MaxOutputBytes = 1;
	TArray<FVaRestJsonTestLargeDefault> Refused;
	Report = {};
	TestFalse(TEXT("controlled storage refusal precedes native default initialization"),
		FVaRestJsonConvert::JsonToProperty(Source, Items, &Refused, Options, &Report));
	TestTrue(TEXT("refused destination remains empty"), Refused.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDomPointerBudgetTest, "VaRest.Json.Budget.DomPointerStorage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDomPointerBudgetTest::RunTest(const FString& Parameters)
{
	FVaRestJson Source = FVaRestJson::Array();
	for (int32 Index = 0; Index < 32; ++Index)
		Source.Add(FString());
	FVaRestJsonConvertOptions Options;
	Options.MaxOutputBytes = 1566;
	AddExpectedError(TEXT("JSON requested/logical output byte limit exceeded"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("root DOM empty-string array charges scratch and final pointer slots"),
		FVaRestJsonConvert::StructToJson(FVaRestJson::StaticStruct(), &Source, Options).IsValid());
	Options.MaxOutputBytes = 4096;
	const FVaRestJson Copy = FVaRestJsonConvert::StructToJson(FVaRestJson::StaticStruct(), &Source, Options);
	if (!TestTrue(TEXT("sufficient allowance produces the whole independent DOM"), Copy.IsArray() && Copy.Num() == 32 && !Copy.IsSameNode(Source)))
		return false;
	for (const FVaRestJson& Item : Copy.Elements())
		TestTrue(TEXT("empty strings retain their type and value"), Item.IsString() && Item.AsString().IsEmpty());
	return true;
}

#endif
