// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonValue.h"
#include "HAL/LowLevelMemTracker.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "VaRestJson.h"

namespace VaRestJsonBench
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

	/**
	 * Bytes still allocated under Tag after Body returns, while Keep (filled by Body) is alive.
	 * -1 when LLM is off: run with VAREST_EXTRA_ARGS=-llm. LLM aggregates per frame, so stats are
	 * flushed by hand around the measurement (LowLevelMemTracker.h:611).
	 */
	template <typename BodyType>
	int64 RetainedBytes(const TCHAR* Tag, BodyType&& Body)
	{
#if ENABLE_LOW_LEVEL_MEM_TRACKER
		if (!FLowLevelMemTracker::IsEnabled())
		{
			return -1;
		}
		const FName TagName(Tag);
		FLowLevelMemTracker& Llm = FLowLevelMemTracker::Get();
		Llm.UpdateStatsPerFrame();
		const int64 Before = Llm.GetTagAmountForTracker(ELLMTracker::Default, TagName, ELLMTagSet::None);
		{
			FLLMScope Scope(TagName, false, ELLMTagSet::None, ELLMTracker::Default);
			Body();
		}
		Llm.UpdateStatsPerFrame();
		return Llm.GetTagAmountForTracker(ELLMTracker::Default, TagName, ELLMTagSet::None) - Before;
#else
		return -1;
#endif
	}

	FString Mb(int64 Bytes)
	{
		return Bytes < 0 ? FString(TEXT("n/a")) : FString::Printf(TEXT("%.1f MiB"), Bytes / (1024.0 * 1024.0));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonBenchTest, "Bench.VaRest.Json.ParseAndWrite",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::PerfFilter)

bool FVaRestJsonBenchTest::RunTest(const FString& Parameters)
{
	using namespace VaRestJsonBench;

	// Calibration: the memory instrument must see a known 8 MiB allocation, or its numbers mean nothing
	TArray<uint8> Calibration;
	const int64 Calibrated = RetainedBytes(TEXT("VaRestBenchCalibration"), [&Calibration] { Calibration.SetNumUninitialized(8 << 20); });
	if (Calibrated >= 0)
	{
		TestTrue(TEXT("LLM sees 8 MiB within 5%"), FMath::Abs(Calibrated - (8 << 20)) < (8 << 20) / 20);
	}

	FString Report = FString::Printf(TEXT("LLM calibration (8 MiB alloc): %s\n\n"), *Mb(Calibrated));
	Report += TEXT("| fixture | path | parse ms (median) | retained memory | write ms |\n|---|---|---|---|---|\n");
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

		// Engine, the v1 way: UTF-8 -> FString first (VaRestRequestJSON.cpp used GetContentAsString)
		TSharedPtr<FJsonValue> EngineValue;
		const double EngineStringMs = MedianMs(Runs, [&] {
			FString Text = FString::ConstructFromPtrSize(Utf8.GetData(), Utf8.Len());
			TSharedPtr<FJsonValue> V;
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), V);
		});
		const int64 EngineMem = RetainedBytes(TEXT("VaRestBenchEngine"), [&] {
			FString Text = FString::ConstructFromPtrSize(Utf8.GetData(), Utf8.Len());
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), EngineValue);
		});

		// Engine reading UTF-8 directly (the fairest engine baseline)
		const double EngineUtf8Ms = MedianMs(Runs, [&] {
			TSharedPtr<FJsonValue> V;
			FJsonSerializer::Deserialize(TJsonReaderFactory<UTF8CHAR>::CreateFromView(Utf8), V);
		});

		// VaRest: copy + in situ, and stolen buffer
		const double OursViewMs = MedianMs(Runs, [&] { auto R = FVaRestJson::Parse(Utf8); (void)R; });
		const double OursOwnedMs = MedianMs(Runs, [&] {
			TArray<uint8> Copy; // the copy is outside what an HTTP response would pay (it reserves the NUL slack byte); subtract below
				Copy.Reserve(Bytes.Num() + 1);
				Copy.Append(Bytes);
			auto R = FVaRestJson::ParseOwned(MoveTemp(Copy));
			(void)R;
		});
		const double CopyMs = MedianMs(Runs, [&] {
				TArray<uint8> Copy;
				Copy.Reserve(Bytes.Num() + 1);
				Copy.Append(Bytes);
			});
		FVaRestJson Ours;
		const int64 OursMem = RetainedBytes(TEXT("VaRestBenchVaRest"), [&] { Ours = FVaRestJson::Parse(Utf8).StealValue(); });

		// Writing
		const double EngineWriteMs = MedianMs(Runs, [&] {
			FString Out;
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(EngineValue, FString(), W);
		});
		const double OursWriteMs = MedianMs(Runs, [&] { TArray<uint8> Out; Ours.ToUtf8(Out); });

		// Item counts, not full equality: the engine reader may round integers beyond 2^53
		TestEqual(TEXT("same item count"), FVaRestJson::FromJsonValue(EngineValue).Path(TEXT("data.items")).Num(), Ours.Path(TEXT("data.items")).Num());

		Report += FString::Printf(TEXT("| %s | engine (FString, v1 path) | %.2f | %s | %.2f |\n"), Name, EngineStringMs, *Mb(EngineMem), EngineWriteMs);
		Report += FString::Printf(TEXT("| %s | engine (UTF-8 reader) | %.2f | | |\n"), Name, EngineUtf8Ms);
		Report += FString::Printf(TEXT("| %s | VaRest Parse (copy + in situ) | %.2f | %s | %.2f |\n"), Name, OursViewMs, *Mb(OursMem), OursWriteMs);
		Report += FString::Printf(TEXT("| %s | VaRest ParseOwned (minus the test's copy) | %.2f | | |\n"), Name, OursOwnedMs - CopyMs);
	}

	AddInfo(Report);
	FFileHelper::SaveStringToFile(Report, *(FPaths::ProjectSavedDir() / TEXT("VaRestBench") / TEXT("results.md")));
	return true;
}

#endif
