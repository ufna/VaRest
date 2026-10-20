// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Containers/StringConv.h"
#include "HAL/PlatformTime.h"
#include "JsonObjectConverter.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonConvertTestTypes.h"

namespace VaRest::Json::ConvertBenchTestPrivate
{
	template <typename BodyType>
	double MedianMs(int32 Runs, BodyType&& Body)
	{
		TArray<double> Times;
		for (int32 i = 0; i < Runs; ++i)
		{
			const double Start = FPlatformTime::Seconds();
			Body();
			Times.Add((FPlatformTime::Seconds() - Start) * 1000.0);
		}
		Times.Sort();
		return Times[Runs / 2];
	}

	/** Folds every field of every item, so a converter that skips a field changes the number */
	int64 Checksum(const FVaRestJsonBenchResponse& R)
	{
		int64 Sum = R.Ok ? 1 : 0;
		Sum += R.Data.Count;
		for (const FVaRestJsonBenchItem& Item : R.Data.Items)
		{
			Sum += Item.Id + Item.Name.Len() * 3 + Item.Desc.Len() * 5 + FMath::RoundToInt64(Item.Price * 100.0)
				+ FMath::RoundToInt64(Item.Weight * 1e6) + (Item.Active ? 7 : 0) + Item.Owner.Id + Item.Owner.Nick.Len() + Item.Tags.Num() * 11;
			for (const FString& Tag : Item.Tags)
			{
				Sum += Tag.Len();
			}
			for (const TPair<FString, int32>& Stat : Item.Stats)
			{
				Sum += Stat.Value + Stat.Key.Len();
			}
		}
		return Sum;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonConvertBenchTest, "Bench.VaRest.Json.TypedConvert",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::PerfFilter)

bool FVaRestJsonConvertBenchTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::ConvertBenchTestPrivate;
	const UScriptStruct* Struct = FVaRestJsonBenchResponse::StaticStruct();
	FString Report = TEXT("| fixture | path | decode ms (median) | encode ms (median) |\n|---|---|---|---|\n");
	FString Notes;
	for (const TCHAR* Name : { TEXT("bench_1mb.json"), TEXT("bench_10mb.json") })
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("VaRestBench") / Name;
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path))
		{
			AddError(FString::Printf(TEXT("missing %s -- run: python3 Tools/gen_bench_json.py <Host>/Saved/VaRestBench"), *Path));
			return false;
		}
		const int32 Runs = Bytes.Num() > (4 << 20) ? 7 : 15;
		const FUtf8StringView Utf8(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());

		// VaRest: UTF-8 bytes -> arena DOM -> struct
		const double OursDecodeMs = MedianMs(Runs, [&] {
			FVaRestJsonBenchResponse Response;
			FVaRestJsonDecodeReport Issues;
			FVaRestJsonConvert::Utf8ToStruct(Bytes, Struct, &Response, FVaRestJsonConvertOptions(), &Issues);
		});
		FVaRestJsonBenchResponse Ours;
		FVaRestJsonDecodeReport OursIssues;
		TestTrue(TEXT("VaRest decodes"), FVaRestJsonConvert::Utf8ToStruct(Bytes, Struct, &Ours, FVaRestJsonConvertOptions(), &OursIssues));
		TestEqual(TEXT("VaRest decode is clean"), OursIssues.Warnings.Num(), 0);

		// Engine, the v1 way: UTF-8 -> FString -> FJsonObject -> struct
		const double EngineDecodeMs = MedianMs(Runs, [&] {
			const FString Text = FString::ConstructFromPtrSize(Utf8.GetData(), Utf8.Len());
			FVaRestJsonBenchResponse Response;
			FJsonObjectConverter::JsonObjectStringToUStruct(Text, &Response);
		});
		// Engine reading UTF-8 directly (the fairest engine baseline)
		const double EngineUtf8DecodeMs = MedianMs(Runs, [&] {
			TSharedPtr<FJsonObject> Object;
			FJsonSerializer::Deserialize(TJsonReaderFactory<UTF8CHAR>::CreateFromView(Utf8), Object);
			FVaRestJsonBenchResponse Response;
			FJsonObjectConverter::JsonObjectToUStruct(Object.ToSharedRef(), &Response);
		});
		FVaRestJsonBenchResponse Engine;
		FJsonObjectConverter::JsonObjectStringToUStruct(FString::ConstructFromPtrSize(Utf8.GetData(), Utf8.Len()), &Engine);
		TestEqual(TEXT("same item count"), Engine.Data.Items.Num(), Ours.Data.Items.Num());
		TestTrue(TEXT("64-bit ids stay exact (generator: 9007199254740993 + i)"), Ours.Data.Items.Num() > 1 && Ours.Data.Items[1].Uid == 9007199254740994ll);
		// Not in the brief: proves both converters fill every field (Uid aside, below), so the timings compare equal work
		TestEqual(TEXT("both converters fill every field (checksum)"), Checksum(Engine), Checksum(Ours));
		// The engine reads every number through a double (FJsonValueNumber), so a 64-bit id above 2^53 comes back rounded.
		// That is a finding, not a check: it is counted into the report and the checksum leaves Uid out.
		int32 EngineInexactUids = 0;
		for (int32 i = 0; i < Engine.Data.Items.Num() && i < Ours.Data.Items.Num(); ++i)
		{
			EngineInexactUids += Engine.Data.Items[i].Uid != Ours.Data.Items[i].Uid ? 1 : 0;
		}

		// Encode: VaRest straight to UTF-8 bytes; the engine to an FString, then the UTF-8 an HTTP body needs
		const double OursEncodeMs = MedianMs(Runs, [&] {
			TArray<uint8> Out;
			FVaRestJsonConvert::StructToUtf8(Struct, &Ours, Out);
		});
		const double EngineEncodeMs = MedianMs(Runs, [&] {
			FString Out;
			FJsonObjectConverter::UStructToJsonObjectString(Struct, &Ours, Out, 0, 0, 0, nullptr, false);
		});
		const double EngineEncodeUtf8Ms = MedianMs(Runs, [&] {
			FString Out;
			FJsonObjectConverter::UStructToJsonObjectString(Struct, &Ours, Out, 0, 0, 0, nullptr, false);
			const FTCHARToUTF8 Body(*Out, Out.Len());
			(void)Body.Length();
		});

		Report += FString::Printf(TEXT("| %s | engine FJsonObjectConverter (FString, v1 path) | %.2f | %.2f |\n"), Name, EngineDecodeMs, EngineEncodeMs);
		Report += FString::Printf(TEXT("| %s | engine (UTF-8 reader; encode + UTF-8 body) | %.2f | %.2f |\n"), Name, EngineUtf8DecodeMs, EngineEncodeUtf8Ms);
		Report += FString::Printf(TEXT("| %s | VaRest FVaRestJsonConvert (UTF-8 in, UTF-8 out) | %.2f | %.2f |\n"), Name, OursDecodeMs, OursEncodeMs);
		Notes += FString::Printf(TEXT("%s: engine decoded %d of %d Uid values (int64 above 2^53) inexactly; VaRest 0.\n"), Name, EngineInexactUids, Ours.Data.Items.Num());
	}
	Report += TEXT("\n") + Notes;
	AddInfo(Report);
	FFileHelper::SaveStringToFile(Report, *(FPaths::ProjectSavedDir() / TEXT("VaRestBench") / TEXT("typed-results.md")));
	return true;
}

#endif
