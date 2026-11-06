// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "VaRestJsonBudget.h"
#include "Misc/AutomationTest.h"
#include "VaRestJson.h"
#include "VaRestJsonReader.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonParseBudgetTest, "VaRest.Json.Budget.Parse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonParseBudgetTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json;
	const auto Parse = [](FUtf8StringView Text, int64 Values, int64 Work) {
		FVaRestJsonParseOptions Options;
		Options.MaxValues = Values;
		Options.MaxWorkUnits = Work;
		return FVaRestJson::Parse(Text, Options);
	};
	TestTrue(TEXT("zero options keep ordinary parse behavior"), Parse(UTF8TEXT("{\"a\":[0,1]}"), 0, 0).HasValue());
	TestTrue(TEXT("root scalar is one value"), Parse(UTF8TEXT("0"), 1, 0).HasValue());
	TestTrue(TEXT("empty container is one value"), Parse(UTF8TEXT("[]"), 1, 0).HasValue());
	TestFalse(TEXT("array root also counts before its scalars"), Parse(UTF8TEXT("[0,0]"), 2, 0).HasValue());
	TestTrue(TEXT("array exact node boundary passes"), Parse(UTF8TEXT("[0,0]"), 3, 0).HasValue());
	TestFalse(TEXT("object keys count separately before their values"), Parse(UTF8TEXT("{\"x\":0}"), 2, 0).HasValue());
	TestTrue(TEXT("object exact node/key boundary passes"), Parse(UTF8TEXT("{\"x\":0}"), 3, 0).HasValue());
	TestFalse(TEXT("nested tiny objects share one count"), Parse(UTF8TEXT("[{\"x\":0},{\"y\":0}]"), 6, 0).HasValue());
	TestFalse(TEXT("negative value limit refuses"), Parse(UTF8TEXT("0"), -1, 0).HasValue());
	TestFalse(TEXT("negative work limit refuses"), Parse(UTF8TEXT("0"), 0, -1).HasValue());
	TestFalse(TEXT("work includes input bytes plus scalar event"), Parse(UTF8TEXT("0"), 0, 1).HasValue());
	TestTrue(TEXT("scalar exact work boundary"), Parse(UTF8TEXT("0"), 0, 2).HasValue());
	TestFalse(TEXT("container close is work too"), Parse(UTF8TEXT("[]"), 0, 3).HasValue());
	TestTrue(TEXT("empty array has two bytes and two events"), Parse(UTF8TEXT("[]"), 0, 4).HasValue());

	FDocument Doc;
	const uint8 Text[] = {'[', '0', ',', '0', ']', 0};
	Doc.Input.Append(Text, UE_ARRAY_COUNT(Text));
	FVaRestJsonParseOptions Options;
	Options.MaxValues = 2;
	FJsonParseUsage Usage;
	FVaRestJsonError Error;
	TestFalse(TEXT("observed actual parser refuses the third node"), ParseInSitu(Doc, 0, Options, {}, Error, nullptr, &Usage));
	TestEqual(TEXT("failed node is not charged"), Usage.Values, int64(2));
	TestEqual(TEXT("only array and first scalar reached arena"), Usage.ArenaUsedBytes, SIZE_T(2 * sizeof(FNode)));
	TestTrue(TEXT("failed parser does not publish a root"), Doc.Root == nullptr);
	TestTrue(TEXT("failure names the real count limit"), Error.Message.Contains(TEXT("value/key")));

	const auto ReusedFailure = [&](FUtf8StringView Source, const FVaRestJsonParseOptions& Limits, const TCHAR* Label) {
		FDocument Reused;
		Reused.Root = Reused.NewInt(42);
		Reused.Input.Append(reinterpret_cast<const uint8*>(Source.GetData()), Source.Len());
		Reused.Input.Add(0);
		const SIZE_T InitialArena = Reused.Arena.GetUsedBytes();
		FJsonParseUsage Observed;
		FVaRestJsonError Failure;
		TestFalse(Label, ParseInSitu(Reused, 0, Limits, {}, Failure, nullptr, &Observed));
		TestTrue(TEXT("all failure paths clear the old root"), Reused.Root == nullptr);
		TestEqual(TEXT("refused parse reports its actual retained input"), Observed.InputAllocatedBytes, Reused.Input.GetAllocatedSize());
		TestTrue(TEXT("refused parse reports preexisting arena ownership"), Observed.ArenaUsedBytes >= InitialArena);
		return Observed;
	};
	Options = {};
	Options.MaxWorkUnits = 1;
	const FJsonParseUsage Early = ReusedFailure(UTF8TEXT("[]"), Options, TEXT("initial work refusal on reused document"));
	TestEqual(TEXT("refused initial compound charge commits no work"), Early.WorkUnits, int64(0));
	TestEqual(TEXT("no builder scratch before admission"), Early.BuilderScratchPeakAllocatedBytes, SIZE_T(0));
	Options = {};
	Options.MaxValues = 1;
	ReusedFailure(UTF8TEXT("[0]"), Options, TEXT("callback refusal on reused document"));
	Options = {};
	ReusedFailure(UTF8TEXT("[x]"), Options, TEXT("syntax refusal on reused document"));
	const UTF8CHAR EmbeddedNull[] = {'[', ']', 0, 'x'};
	ReusedFailure(FUtf8StringView(EmbeddedNull, UE_ARRAY_COUNT(EmbeddedNull)), Options, TEXT("trailing NUL refusal on reused document"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonParseOwnershipBudgetTest, "VaRest.Json.Budget.ZeroArrayOwnership",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonParseOwnershipBudgetTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json;
	constexpr int32 Count = 1000000;
	FDocument Doc;
	Doc.Input.Reserve(Count * 2 + 2);
	Doc.Input.Add('[');
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (Index)
			Doc.Input.Add(',');
		Doc.Input.Add('0');
	}
	Doc.Input.Add(']');
	Doc.Input.Add(0);
	FVaRestJsonParseOptions Options;
	// Deliberately above the dynamic default, so this fixture measures the shape rather than its admission refusal.
	Options.MaxValues = Count + 1;
	FJsonParseUsage Usage;
	FVaRestJsonError Error;
	if (!TestTrue(TEXT("measurement parses the real million-zero DOM"), ParseInSitu(Doc, 0, Options, {}, Error, nullptr, &Usage)))
		return false;
	TestEqual(TEXT("million scalars plus root"), Usage.Values, int64(Count + 1));
	TestTrue(TEXT("node/vector ownership exceeds input-based estimate"),
		Usage.ArenaUsedBytes + Usage.BuilderScratchPeakAllocatedBytes > SIZE_T(8 * (Doc.Input.Num() - 1) + 2 * 1024 * 1024));
	TestTrue(TEXT("scratch includes a pointer for every array entry"), Usage.BuilderScratchPeakAllocatedBytes >= SIZE_T(Count) * sizeof(FNode*));
	AddInfo(FString::Printf(TEXT("Observed arena used=%llu reserved=%llu input allocated=%llu builder scratch high-water=%llu; excludes RapidJSON stack/allocator overhead/RSS"),
		static_cast<uint64>(Usage.ArenaUsedBytes), static_cast<uint64>(Usage.ArenaReservedBytes),
		static_cast<uint64>(Usage.InputAllocatedBytes), static_cast<uint64>(Usage.BuilderScratchPeakAllocatedBytes)));
	return true;
}

#endif
