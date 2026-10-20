// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonConvertTestTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonTypedGetTest, "VaRest.Json.Api.TryGetTyped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonTypedGetTest::RunTest(const FString& Parameters)
{
	const FVaRestJson J = FVaRestJson::Parse(FStringView(TEXT(
		"{\"inner\":{\"id\":7,\"name\":\"Neo\"},\"nums\":[1,2,3],\"mixed\":[1,\"x\"],\"items\":[{\"id\":1},{\"id\":2}],"
		"\"scores\":{\"a\":1,\"b\":2},\"bad\":{\"a\":\"x\"},\"pos\":{\"x\":1,\"y\":2,\"z\":3},\"n\":null}"))).StealValue();

	const TOptional<FVaRestJsonTestInner> Inner = J[TEXT("inner")].TryGet<FVaRestJsonTestInner>();
	TestTrue(TEXT("a USTRUCT"), Inner.IsSet() && Inner->Id == 7 && Inner->Name.Equals(TEXT("Neo"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("null is not a struct"), J[TEXT("n")].TryGet<FVaRestJsonTestInner>().IsSet());
	TestFalse(TEXT("a missing value is not a struct"), J[TEXT("nope")].TryGet<FVaRestJsonTestInner>().IsSet());
	TestFalse(TEXT("an array is not a struct"), J[TEXT("nums")].TryGet<FVaRestJsonTestInner>().IsSet());
	// Review Focus: a value struct is not set from null (its field decode keeps the value on null)
	TestFalse(TEXT("null is not a date"), J[TEXT("n")].TryGet<FDateTime>().IsSet());
	TestFalse(TEXT("null is not a GUID"), J[TEXT("n")].TryGet<FGuid>().IsSet());
	TestFalse(TEXT("a number is not a GUID"), J.Path(TEXT("inner.id")).TryGet<FGuid>().IsSet());

	const TOptional<TArray<int32>> Nums = J[TEXT("nums")].TryGet<TArray<int32>>();
	TestTrue(TEXT("TArray<int32>"), Nums.IsSet() && *Nums == TArray<int32>({ 1, 2, 3 }));
	TestFalse(TEXT("one bad element fails the array"), J[TEXT("mixed")].TryGet<TArray<int32>>().IsSet());
	const TOptional<TArray<FVaRestJsonTestInner>> Items = J[TEXT("items")].TryGet<TArray<FVaRestJsonTestInner>>();
	TestTrue(TEXT("TArray of structs"), Items.IsSet() && Items->Num() == 2 && (*Items)[1].Id == 2);

	const TOptional<TMap<FString, int32>> Scores = J[TEXT("scores")].TryGet<TMap<FString, int32>>();
	TestTrue(TEXT("TMap<FString, int32>"), Scores.IsSet() && Scores->Num() == 2 && Scores->FindRef(TEXT("b")) == 2);
	TestFalse(TEXT("one bad value fails the map"), J[TEXT("bad")].TryGet<TMap<FString, int32>>().IsSet());

	const TOptional<FVector> Pos = J[TEXT("pos")].TryGet<FVector>();
	TestTrue(TEXT("an engine struct through TBaseStructure"), Pos.IsSet() && *Pos == FVector(1, 2, 3));

	FVaRestJsonTestInner Fallback;
	Fallback.Id = -1;
	TestEqual(TEXT("As<T> gives the default on a miss"), J[TEXT("nope")].As<FVaRestJsonTestInner>(Fallback).Id, -1);
	TestEqual(TEXT("As<TArray>"), J[TEXT("nums")].As<TArray<int32>>().Num(), 3);
	TestEqual(TEXT("As<int32> like AsInt"), J.Path(TEXT("inner.id")).As<int32>(0), 7);

	const TValueOrError<FVaRestJsonTestInner, FVaRestJsonError> Decoded = FVaRestJsonConvert::FromJson<FVaRestJsonTestInner>(J[TEXT("inner")]);
	TestTrue(TEXT("FromJson<T>"), Decoded.HasValue() && Decoded.GetValue().Id == 7);
	FVaRestJsonConvertOptions Strict;
	Strict.bStrict = true;
	const TValueOrError<FVaRestJsonTestInner, FVaRestJsonError> Failed =
		FVaRestJsonConvert::FromJson<FVaRestJsonTestInner>(FVaRestJson::Parse(FUtf8StringView(UTF8TEXT("{\"id\":\"7\"}"))).StealValue(), Strict);
	TestTrue(TEXT("strict FromJson reports the error with its path"), Failed.HasError() && Failed.GetError().Path.Equals(TEXT("id"), ESearchCase::CaseSensitive));

	// Value structs: only their scalar form is a value
	const FVaRestJson V = FVaRestJson::Parse(FStringView(TEXT(
		"{\"span\":90.5,\"spanBig\":1e30,\"spanStr\":\"5\",\"obj\":\"/Game/A.A\",\"cls\":\"/Script/Engine.Actor\",\"num\":3,"
		"\"junk\":\"garbage\",\"dates\":[\"2026-10-20T12:00:00Z\",null],\"ok\":\"2026-10-20T12:00:00Z\","
		"\"guid\":\"00000000-0000-0000-0000-000000000001\",\"ci\":{\"a\":1,\"A\":2},\"wrong\":{\"id\":\"7\"},\"empty\":{}}"))).StealValue();
	const TOptional<FTimespan> Span = V[TEXT("span")].TryGet<FTimespan>();
	TestTrue(TEXT("FTimespan from seconds"), Span.IsSet() && Span->GetTotalSeconds() == 90.5);
	TestFalse(TEXT("out-of-range seconds are not a timespan"), V[TEXT("spanBig")].TryGet<FTimespan>().IsSet());
	TestFalse(TEXT("null is not a timespan"), J[TEXT("n")].TryGet<FTimespan>().IsSet());
	TestFalse(TEXT("a string is not a timespan"), V[TEXT("spanStr")].TryGet<FTimespan>().IsSet());
	TestFalse(TEXT("an object is not a timespan"), V[TEXT("wrong")].TryGet<FTimespan>().IsSet());
	TestTrue(TEXT("FromJson<FTimespan>"), FVaRestJsonConvert::FromJson<FTimespan>(V[TEXT("span")]).HasValue());
	TestTrue(TEXT("FromJson<FTimespan> out of range fails"), FVaRestJsonConvert::FromJson<FTimespan>(V[TEXT("spanBig")]).HasError());

	TestTrue(TEXT("soft object path"), V[TEXT("obj")].TryGet<FSoftObjectPath>().IsSet());
	TestTrue(TEXT("soft class path"), V[TEXT("cls")].TryGet<FSoftClassPath>().IsSet());
	TestFalse(TEXT("a number is not a soft path"), V[TEXT("num")].TryGet<FSoftObjectPath>().IsSet());
	TestFalse(TEXT("null is not a soft path"), J[TEXT("n")].TryGet<FSoftObjectPath>().IsSet());
	TestFalse(TEXT("null is not a soft class path"), J[TEXT("n")].TryGet<FSoftClassPath>().IsSet());

	TestTrue(TEXT("a date"), V[TEXT("ok")].TryGet<FDateTime>().IsSet());
	TestFalse(TEXT("garbage is not a date"), V[TEXT("junk")].TryGet<FDateTime>().IsSet());
	TestFalse(TEXT("garbage is not a GUID"), V[TEXT("junk")].TryGet<FGuid>().IsSet());
	TestTrue(TEXT("a GUID"), V[TEXT("guid")].TryGet<FGuid>().IsSet());
	TestFalse(TEXT("a null element fails TArray<FDateTime>"), V[TEXT("dates")].TryGet<TArray<FDateTime>>().IsSet());

	const TOptional<TMap<FString, int32>> Folded = V[TEXT("ci")].TryGet<TMap<FString, int32>>();
	TestTrue(TEXT("case-variant keys collapse, the last wins"), Folded.IsSet() && Folded->Num() == 1 && Folded->FindRef(TEXT("a")) == 2);
	TestEqual(TEXT("As<TMap>"), J[TEXT("scores")].As<TMap<FString, int32>>().Num(), 2);

	// A struct's fields decode tolerantly (default options); FromJson with bStrict is the strict path
	const TOptional<FVaRestJsonTestInner> Tolerant = V[TEXT("wrong")].TryGet<FVaRestJsonTestInner>();
	TestTrue(TEXT("TryGet<Struct> reads a numeric string field leniently"), Tolerant.IsSet() && Tolerant->Id == 7);
	const TOptional<FVaRestJsonTestInner> Defaults = V[TEXT("empty")].TryGet<FVaRestJsonTestInner>();
	TestTrue(TEXT("{} gives the defaults"), Defaults.IsSet() && Defaults->Id == FVaRestJsonTestInner().Id);

	const TValueOrError<FVaRestJsonTestInner, FVaRestJsonError> FromNull = FVaRestJsonConvert::FromJson<FVaRestJsonTestInner>(J[TEXT("n")]);
	TestTrue(TEXT("FromJson on null gives a default T"), FromNull.HasValue() && FromNull.GetValue().Id == FVaRestJsonTestInner().Id);
	TestTrue(TEXT("FromJson on a missing value is an error"), FVaRestJsonConvert::FromJson<FVaRestJsonTestInner>(J[TEXT("nope")]).HasError());
	return true;
}

#endif
