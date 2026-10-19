// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/LowLevelMemTracker.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "VaRestJsonDocument.h"
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

	// Second calibration: many small allocations (what the DOM does), allocator overhead may count
	TArray<void*> Small;
	Small.Reserve(100000); // reserved outside the tagged scope so only the 64 B blocks are counted
	const int64 SmallBytes = RetainedBytes(TEXT("VaRestBenchSmall"), [&Small] {
		for (int32 i = 0; i < 100000; ++i)
		{
			Small.Add(FMemory::Malloc(64));
		}
	});
	if (SmallBytes >= 0)
	{
		TestTrue(TEXT("LLM sees 100k x 64 B within 10%"), FMath::Abs(SmallBytes - 6400000) < 640000);
	}
	const FString SmallLine = FString::Printf(TEXT("LLM calibration (100000 x 64 B, expect 6400000 B): %lld B\n\n"), SmallBytes);
	for (void* P : Small)
	{
		FMemory::Free(P);
	}

	FString ArenaReport;
	FString Report = FString::Printf(TEXT("LLM calibration (8 MiB alloc): %s\n\n"), *Mb(Calibrated));
	Report += SmallLine;
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
		// The input copy reserves the NUL slack byte, as the HTTP layer will; its cost is measured next and subtracted.
		const double OursOwnedMs = MedianMs(Runs, [&] {
			TArray<uint8> Copy;
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

		ArenaReport += FString::Printf(TEXT("arena %s: input %lld B, used %llu B, reserved %llu B, used/input %.3f, reserved/input %.3f\n\n"), Name,
			(int64)Bytes.Num(), (uint64)Ours.GetDocumentInternal()->Arena.GetUsedBytes(), (uint64)Ours.GetDocumentInternal()->Arena.GetReservedBytes(),
			(double)Ours.GetDocumentInternal()->Arena.GetUsedBytes() / Bytes.Num(), (double)Ours.GetDocumentInternal()->Arena.GetReservedBytes() / Bytes.Num());

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

	Report += TEXT("\n") + ArenaReport;

	// Many small documents kept alive (SaveGame fields, pin defaults, small API responses): what each
	// one retains. The arrays are reserved outside the tagged scope so only the documents are counted.
	{
		const int32 Count = 1000;
		const FUtf8StringView SmallText(UTF8TEXT("{\"id\":1,\"name\":\"x\"}"));
		const FString SmallString = FString::ConstructFromPtrSize(SmallText.GetData(), SmallText.Len());

		TArray<TSharedPtr<FJsonObject>> EngineDocs;
		EngineDocs.Reserve(Count);
		const int64 EngineSmallMem = RetainedBytes(TEXT("VaRestBenchSmallDocEngine"), [&] {
			for (int32 i = 0; i < Count; ++i)
			{
				TSharedPtr<FJsonObject> Object;
				FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SmallString), Object);
				EngineDocs.Add(MoveTemp(Object));
			}
		});
		TArray<FVaRestJson> OurDocs;
		OurDocs.Reserve(Count);
		const int64 OursSmallMem = RetainedBytes(TEXT("VaRestBenchSmallDocVaRest"), [&] {
			for (int32 i = 0; i < Count; ++i)
			{
				OurDocs.Add(FVaRestJson::Parse(SmallText).StealValue());
			}
		});
		const double EngineSmallMs = MedianMs(15, [&] {
			for (int32 i = 0; i < Count; ++i)
			{
				TSharedPtr<FJsonObject> Object;
				FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SmallString), Object);
			}
		});
		const double OursSmallMs = MedianMs(15, [&] {
			for (int32 i = 0; i < Count; ++i)
			{
				auto R = FVaRestJson::Parse(SmallText);
				(void)R;
			}
		});
		TestTrue(TEXT("small docs parsed"), EngineDocs.Num() == Count && EngineDocs.Last().IsValid() && OurDocs.Num() == Count && OurDocs.Last()[TEXT("id")].AsInt() == 1);

		const auto PerDoc = [Count](int64 Bytes) { return Bytes < 0 ? FString(TEXT("n/a")) : FString::Printf(TEXT("%lld B"), Bytes / Count); };
		Report += FString::Printf(TEXT("| small document x%d (`%s`) | path | parse ms for all %d (median of 15) | retained per document |\n|---|---|---|---|\n"), Count, *SmallString, Count);
		Report += FString::Printf(TEXT("| | engine FJsonObject (FString reader) | %.3f | %s |\n"), EngineSmallMs, *PerDoc(EngineSmallMem));
		Report += FString::Printf(TEXT("| | VaRest Parse | %.3f | %s |\n"), OursSmallMs, *PerDoc(OursSmallMem));
		Report += FString::Printf(TEXT("\nsmall document arena: used %llu B, reserved %llu B; input buffer %llu B allocated\n"),
			(uint64)OurDocs[0].GetDocumentInternal()->Arena.GetUsedBytes(), (uint64)OurDocs[0].GetDocumentInternal()->Arena.GetReservedBytes(),
			(uint64)OurDocs[0].GetDocumentInternal()->Input.GetAllocatedSize());
	}

	AddInfo(Report);
	FFileHelper::SaveStringToFile(Report, *(FPaths::ProjectSavedDir() / TEXT("VaRestBench") / TEXT("results.md")));
	return true;
}

#endif
