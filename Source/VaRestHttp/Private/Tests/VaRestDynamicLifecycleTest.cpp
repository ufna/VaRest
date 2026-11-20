// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestDynamicLifecycleTestTypes.h"
#include <atomic>

namespace VaRest::Http::DynamicLifecycleTest
{
namespace
{
std::atomic<bool> Tracking{false};
std::atomic<int64> Counts[static_cast<uint8>(EOperation::Count)]{};
std::atomic<int64> WrongThread{0};
void Record(EOperation Operation)
{
	if (!Tracking.load(std::memory_order_acquire)) return;
	Counts[static_cast<uint8>(Operation)].fetch_add(1, std::memory_order_relaxed);
	if (!IsInGameThread()) WrongThread.fetch_add(1, std::memory_order_relaxed);
}
}
void BeginObservationOnGT()
{
	check(IsInGameThread());
	for (auto& Count : Counts) Count.store(0, std::memory_order_relaxed);
	WrongThread.store(0, std::memory_order_relaxed);
	Tracking.store(true, std::memory_order_release);
}
FObservation ObserveOnGT()
{
	check(IsInGameThread()); FObservation Result;
	for (uint8 Index = 0; Index < static_cast<uint8>(EOperation::Count); ++Index)
		Result.Operations[Index] = Counts[Index].load(std::memory_order_relaxed);
	Result.WrongThread = WrongThread.load(std::memory_order_relaxed);
	return Result;
}
void EndObservationOnGT() { check(IsInGameThread()); Tracking.store(false, std::memory_order_release); }
}

using VaRest::Http::DynamicLifecycleTest::EOperation;
FVaRestDynamicLifecycleLeaf::FVaRestDynamicLifecycleLeaf() : Text(TEXT("native-leaf"))
{ VaRest::Http::DynamicLifecycleTest::Record(EOperation::DefaultConstruct); }
FVaRestDynamicLifecycleLeaf::FVaRestDynamicLifecycleLeaf(const FVaRestDynamicLifecycleLeaf& Other) : Text(Other.Text), Number(Other.Number)
{ VaRest::Http::DynamicLifecycleTest::Record(EOperation::CopyConstruct); }
FVaRestDynamicLifecycleLeaf& FVaRestDynamicLifecycleLeaf::operator=(const FVaRestDynamicLifecycleLeaf& Other)
{
	VaRest::Http::DynamicLifecycleTest::Record(EOperation::Assign);
	if (this != &Other) { Text = Other.Text; Number = Other.Number; }
	return *this;
}
FVaRestDynamicLifecycleLeaf::~FVaRestDynamicLifecycleLeaf()
{ VaRest::Http::DynamicLifecycleTest::Record(EOperation::Destroy); }
bool FVaRestDynamicLifecycleLeaf::operator==(const FVaRestDynamicLifecycleLeaf& Other) const
{
	VaRest::Http::DynamicLifecycleTest::Record(EOperation::Equal);
	return Text == Other.Text && Number == Other.Number;
}
uint32 GetTypeHash(const FVaRestDynamicLifecycleLeaf& Value)
{
	VaRest::Http::DynamicLifecycleTest::Record(EOperation::Hash);
	return HashCombine(GetTypeHash(Value.Text), GetTypeHash(Value.Number));
}
FVaRestDynamicLifecycleRoot::FVaRestDynamicLifecycleRoot() : Name(TEXT("native-default"))
{ VaRest::Http::DynamicLifecycleTest::Record(EOperation::DefaultConstruct); }
FVaRestDynamicLifecycleRoot::FVaRestDynamicLifecycleRoot(const FVaRestDynamicLifecycleRoot& Other) : Name(Other.Name), Maybe(Other.Maybe)
{ VaRest::Http::DynamicLifecycleTest::Record(EOperation::CopyConstruct); }
FVaRestDynamicLifecycleRoot& FVaRestDynamicLifecycleRoot::operator=(const FVaRestDynamicLifecycleRoot& Other)
{
	VaRest::Http::DynamicLifecycleTest::Record(EOperation::Assign);
	if (this != &Other) { Name = Other.Name; Maybe = Other.Maybe; }
	return *this;
}
FVaRestDynamicLifecycleRoot::~FVaRestDynamicLifecycleRoot()
{ VaRest::Http::DynamicLifecycleTest::Record(EOperation::Destroy); }
bool FVaRestDynamicLifecycleRoot::operator==(const FVaRestDynamicLifecycleRoot& Other) const
{
	VaRest::Http::DynamicLifecycleTest::Record(EOperation::Equal);
	return Name == Other.Name && Maybe == Other.Maybe;
}
uint32 GetTypeHash(const FVaRestDynamicLifecycleRoot& Value)
{
	VaRest::Http::DynamicLifecycleTest::Record(EOperation::Hash);
	return HashCombine(GetTypeHash(Value.Name), Value.Maybe.IsSet() ? GetTypeHash(Value.Maybe.GetValue()) : 0u);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "VaRestDynamicClosure.h"
#include "VaRestDynamicTypes.h"
#include "VaRestHttpTesting.h"
#include "CoreGlobals.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonRules.h"
#include "Misc/AutomationTest.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyOptional.h"

namespace VaRest::Http::DynamicLifecycleTest
{
class FQualification final : public IAutomationLatentCommand
{
public:
	explicit FQualification(FAutomationTestBase& InTest) : Test(InTest), Deadline(FPlatformTime::Seconds() + 15.0), CarrierBaseline(Testing::LiveDynamicCarriersOnGT()) {}
	~FQualification() override { Cleanup(); }
	bool Update() override
	{
		check(IsInGameThread());
		if (FPlatformTime::Seconds() >= Deadline) { Test.AddError(TEXT("Optional lifecycle qualification did not settle on real engine frames")); Cleanup(); return true; }
		if (Phase == 0)
		{
			if (!Intrinsic()) return false;
			const auto* Optional = FindFProperty<FOptionalProperty>(FVaRestDynamicLifecycleRoot::StaticStruct(), GET_MEMBER_NAME_CHECKED(FVaRestDynamicLifecycleRoot, Maybe));
			const auto* Inner = Optional ? CastField<FStructProperty>(Optional->GetValueProperty()) : nullptr;
			if (!Test.TestTrue(TEXT("real generated Optional owns the only nested native leaf"), Inner && Inner->Struct == FVaRestDynamicLifecycleLeaf::StaticStruct())) return true;
			bRoot = FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestDynamicLifecycleRoot::StaticStruct(), TEXT("VaRest.Test.GT.Optional.Root.v1"));
			if (!Test.TestTrue(TEXT("register root without an explicit dependency masking Optional"), bRoot)) return true;
			Advance(); return false;
		}
		if (GFrameCounter <= PublicationFrame || !Intrinsic()) return false;
		if (Phase == 1)
		{
			const auto Refused = Capture(EVaRestTypeContainer::None);
			Test.TestFalse(TEXT("an unqualified native type reached only through Optional refuses activation"), Refused.IsValid());
			Test.TestTrue(TEXT("refusal is the actual native lifecycle policy"), Error.Contains(TEXT("qualified game-thread lifecycle contract")));
			bLeaf = FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestDynamicLifecycleLeaf::StaticStruct(), TEXT("VaRest.Test.GT.Optional.Leaf.v1"));
			if (!Test.TestTrue(TEXT("register actual Optional leaf"), bLeaf)) { Cleanup(); return true; }
			Advance(); return false;
		}
		if (Phase == 2)
		{
			BeginObservationOnGT(); bObserving = true;
			Destination = MakeUnique<FVaRestDynamicLifecycleDestinations>();
			const auto Json = Object(TEXT("alpha"), true);
			const auto* Scalar = FindFProperty<FStructProperty>(FVaRestDynamicLifecycleDestinations::StaticStruct(), GET_MEMBER_NAME_CHECKED(FVaRestDynamicLifecycleDestinations, Scalar));
			const auto* Array = FindFProperty<FArrayProperty>(FVaRestDynamicLifecycleDestinations::StaticStruct(), GET_MEMBER_NAME_CHECKED(FVaRestDynamicLifecycleDestinations, Array));
			const auto* Map = FindFProperty<FMapProperty>(FVaRestDynamicLifecycleDestinations::StaticStruct(), GET_MEMBER_NAME_CHECKED(FVaRestDynamicLifecycleDestinations, Map));
			if (!Test.TestTrue(TEXT("all actual generated destination properties exist"), Scalar && Array && Map)) { Cleanup(); return true; }
			if (bMeasureComponents && !MeasureComponents(Json, Scalar)) { Cleanup(); return true; }
			Finalize(EVaRestTypeContainer::None, Json, Scalar, &Destination->Scalar, true);
			TestValue(Destination->Scalar, TEXT("alpha"), true);
			Finalize(EVaRestTypeContainer::Array, FVaRestJson::Array().Add(Json).Add(Object(TEXT("second"), false)), Array, &Destination->Array, true);
			if (Test.TestEqual(TEXT("real native array has two values"), Destination->Array.Num(), 2))
			{ TestValue(Destination->Array[0], TEXT("alpha"), true); TestValue(Destination->Array[1], TEXT("second"), false); }
			Finalize(EVaRestTypeContainer::Map, FVaRestJson::Object().Set(TEXT("one"), Json), Map, &Destination->Map, true);
			if (const auto* One = Destination->Map.Find(TEXT("one"))) TestValue(*One, TEXT("alpha"), true);
			else Test.AddError(TEXT("real string map did not receive its native value"));
			// A genuine incompatible descriptor refuses before touching the already prepared scalar.
			Finalize(EVaRestTypeContainer::None, Json, Array, &Destination->Scalar, false);
			TestValue(Destination->Scalar, TEXT("alpha"), true);
			{
				FVaRestDynamicLifecycleRoot Copy(Destination->Scalar);
				FVaRestDynamicLifecycleRoot::StaticStruct()->CopyScriptStruct(&Copy, &Destination->Scalar);
				Test.TestTrue(TEXT("actual native struct equality sees the copied value"), FVaRestDynamicLifecycleRoot::StaticStruct()->CompareScriptStruct(&Copy, &Destination->Scalar, 0));
				Test.TestEqual(TEXT("actual native hash agrees after copy"), FVaRestDynamicLifecycleRoot::StaticStruct()->GetStructTypeHash(&Copy), FVaRestDynamicLifecycleRoot::StaticStruct()->GetStructTypeHash(&Destination->Scalar));
				Copy.Name = TEXT("different");
				Test.TestFalse(TEXT("actual native equality detects a different value"), FVaRestDynamicLifecycleRoot::StaticStruct()->CompareScriptStruct(&Copy, &Destination->Scalar, 0));
			}
			Cleanup();
			const auto Seen = ObserveOnGT();
			for (const auto Operation : {EOperation::DefaultConstruct, EOperation::CopyConstruct, EOperation::Assign, EOperation::Destroy, EOperation::Equal, EOperation::Hash})
				Test.TestTrue(TEXT("each real native initialization/copy/assignment/destruction/equality/hash operation ran"), Seen.Operations[static_cast<uint8>(Operation)] > 0);
			Test.TestEqual(TEXT("all observed actual native value operations ran on GT"), Seen.WrongThread, int64(0));
			Test.TestEqual(TEXT("all tracked reflected carriers return to the real baseline"), Testing::LiveDynamicCarriersOnGT(), CarrierBaseline);
			if (bMeasureComponents)
			{
				auto Operations = FVaRestJson::Object();
				const TCHAR* Names[] = {TEXT("default_construct"), TEXT("copy_construct"), TEXT("assign"), TEXT("destroy"), TEXT("equal"), TEXT("hash")};
				for (uint8 Index = 0; Index < static_cast<uint8>(EOperation::Count); ++Index) Operations.Set(Names[Index], Seen.Operations[Index]);
				CostReport.Set(TEXT("native_operations_total"), Operations).Set(TEXT("wrong_thread_total"), Seen.WrongThread)
					.Set(TEXT("tracked_carriers_after_cleanup"), Testing::LiveDynamicCarriersOnGT());
			}
			Advance(); return false;
		}
		if (bMeasureComponents) WriteComponentReport();
		return true; // Intrinsic() above proves cleanup has really settled.
	}
private:
	static FVaRestJson Object(const TCHAR* Name, bool bSet)
	{
		auto Value = FVaRestJson::Object().Set(TEXT("name"), Name);
		if (bSet) Value.Set(TEXT("maybe"), FVaRestJson::Object().Set(TEXT("text"), TEXT("nested")).Set(TEXT("number"), 23));
		return Value;
	}
	void TestValue(const FVaRestDynamicLifecycleRoot& Value, const TCHAR* Name, bool bSet)
	{
		Test.TestEqual(TEXT("copied native name"), Value.Name, FString(Name));
		Test.TestEqual(TEXT("copied Optional presence"), Value.Maybe.IsSet(), bSet);
		if (bSet && Value.Maybe.IsSet())
		{ Test.TestEqual(TEXT("copied Optional text"), Value.Maybe->Text, FString(TEXT("nested"))); Test.TestEqual(TEXT("copied Optional number"), Value.Maybe->Number, 23); }
	}
	TSharedPtr<FDynamicTypedState, ESPMode::ThreadSafe> Intrinsic()
	{
		FVaRestDynamicResultConfig Config; Config.ResponseType.Kind = EVaRestTypeKind::Struct; Config.ResponseType.Type = FVaRestJson::StaticStruct();
		return FDynamicTypedState::CreateOnGT(Config, Error);
	}
	TSharedPtr<FDynamicTypedState, ESPMode::ThreadSafe> Capture(EVaRestTypeContainer Container)
	{
		FVaRestDynamicResultConfig Config; Config.ResponseType.Kind = EVaRestTypeKind::Struct;
		Config.ResponseType.Type = FVaRestDynamicLifecycleRoot::StaticStruct(); Config.ResponseType.Container = Container;
		return FDynamicTypedState::CreateOnGT(Config, Error);
	}
	void Finalize(EVaRestTypeContainer Container, const FVaRestJson& Json, const FProperty* Property, void* Address, bool bExpected)
	{
		auto State = Capture(Container);
		if (!Test.TestTrue(TEXT("qualified Optional closure creates the real state"), State.IsValid())) return;
		FVaRestDynamicLimits Limits; auto Budget = FVaRestDynamicBudget::Create(Limits, Error); FVaRestDynamicBudgetLease Lease;
		if (!Test.TestTrue(TEXT("real dynamic output admission"), Budget.IsValid() && Budget->TryReserve(Lease))) return;
		int32 Prepared = 0;
		Test.TestTrue(TEXT("real private preparation binds once"), State->SetPreparationOnGT([&](FDynamicPrepareToken& Token)
		{
			++Prepared;
			Test.TestFalse(TEXT("successful response is not typed error preparation"), Token.IsError());
			Test.TestEqual(TEXT("real native carrier exists during preparation"), Testing::LiveDynamicCarriersOnGT(), CarrierBaseline + 1);
			Test.TestEqual(TEXT("actual Take matches the generated destination"), Token.Take(Property, Address), bExpected);
			Test.TestEqual(TEXT("Take resets the real carrier before returning"), Testing::LiveDynamicCarriersOnGT(), CarrierBaseline);
			Test.TestFalse(TEXT("Take cannot publish a second copy"), Token.Take(Property, Address));
			Test.TestEqual(TEXT("confirmation requires actual successful transfer"), Token.ConfirmPrepared(), bExpected);
			Test.TestFalse(TEXT("confirmation cannot publish twice"), Token.ConfirmPrepared());
		}));
		FFinish Finish; FVaRestDynamicTypeInfo Response, ErrorInfo;
		State->FinalizeOnGT(Finish, Json, Response, ErrorInfo, Limits, Lease);
		Test.TestEqual(TEXT("one synchronous preparation"), Prepared, 1);
		Test.TestEqual(TEXT("availability reflects the actual copy outcome"), Response.Availability, bExpected ? EVaRestDynamicAvailability::Available : EVaRestDynamicAvailability::Unavailable);
		Test.TestEqual(TEXT("failed copy is a real failure, not typed Success"), Finish.Error.IsError(), !bExpected);
		if (!bExpected) Test.TestEqual(TEXT("failed destination has a bounded actual error"), Finish.Error.Code, FString(TEXT("DynamicOutputPreparationFailed")));
		Test.TestFalse(TEXT("finalized state cannot bind a late preparation"), State->SetPreparationOnGT([](FDynamicPrepareToken&) {}));
		Test.TestEqual(TEXT("native finalizer has no retained reflected carrier"), Testing::LiveDynamicCarriersOnGT(), CarrierBaseline);
		Lease.Reset(); Test.TestEqual(TEXT("owned test output admission releases"), Budget->NumPending(), 0);
	}
	static bool ComponentValueMatches(const FVaRestDynamicLifecycleRoot& Value)
	{
		return Value.Name == TEXT("alpha") && Value.Maybe.IsSet() &&
			Value.Maybe->Text == TEXT("nested") && Value.Maybe->Number == 23;
	}
	static FVaRestJson Samples(TArray<double> Values)
	{
		auto Raw = FVaRestJson::Array();
		for (double Value : Values) Raw.Add(Value);
		Values.Sort();
		return FVaRestJson::Object().Set(TEXT("median_seconds"), Values[Values.Num() / 2]).Set(TEXT("samples_seconds"), Raw);
	}
	bool MeasureComponents(const FVaRestJson& Json, const FStructProperty* Property)
	{
		check(IsInGameThread());
		if (!Test.TestTrue(TEXT("opt-in component report requires an explicit absolute JSON path"),
			FParse::Value(FCommandLine::Get(), TEXT("VaRestM5GTComponentCostReport="), CostReportPath) &&
			!FPaths::IsRelative(CostReportPath) && FPaths::GetExtension(CostReportPath) == TEXT("json"))) return false;
		constexpr int32 Warmup = 1, Repetitions = 31;
		const auto* Type = FVaRestDynamicLifecycleRoot::StaticStruct();
		TArray<uint8> Input; Json.ToUtf8(Input);
		FVaRestDynamicLimits Limits; auto Budget = FVaRestDynamicBudget::Create(Limits, Error);
		if (!Test.TestTrue(TEXT("opt-in uses the real finite dynamic budget"), Budget.IsValid())) return false;
		FVaRestJsonConvertOptions Options; Options.Rules = FVaRestJsonRules::DefaultSnapshot();
		Options.MaxOutputBytes = Limits.MaxOutputBytes; Options.MaxValues = Limits.MaxValues; Options.MaxWorkUnits = Limits.MaxWorkUnits;
		TArray<double> Initialize, Convert, Copy, Reset, TakeCombined;
		int32 SemanticSamples = 0, PreparedSamples = 0, ConfirmedSamples = 0, ReleasedSamples = 0;
		int32 PeakTrackedCarriers = CarrierBaseline;
		int64 PeakReservedOutput = 0;
		for (int32 Iteration = -Warmup; Iteration < Repetitions; ++Iteration)
		{
			// Prebuilt JSON/destination, checks, sample storage and setup are outside each timed span.
			FVaRestDynamicLifecycleRoot Copied;
			FInstancedStruct Carrier;
			double Start = FPlatformTime::Seconds();
			Carrier.InitializeAs(Type);
			const double InitializeSeconds = FPlatformTime::Seconds() - Start;
			const auto* Initialized = reinterpret_cast<const FVaRestDynamicLifecycleRoot*>(Carrier.GetMemory());
			if (!Test.TestTrue(TEXT("timed initialization preserves the actual native defaults"),
				Carrier.IsValid() && Initialized->Name == TEXT("native-default") && !Initialized->Maybe.IsSet())) return false;
			FVaRestJsonDecodeReport Issues;
			Start = FPlatformTime::Seconds();
			const bool bConverted = FVaRestJsonConvert::JsonToStruct(Json, Type, Carrier.GetMutableMemory(), Options, &Issues);
			const double ConvertSeconds = FPlatformTime::Seconds() - Start;
			if (!Test.TestTrue(TEXT("timed conversion materializes all qualified Optional values"),
				bConverted && Issues.Errors.IsEmpty() && Issues.Warnings.IsEmpty() &&
				ComponentValueMatches(*reinterpret_cast<const FVaRestDynamicLifecycleRoot*>(Carrier.GetMemory())))) return false;
			Start = FPlatformTime::Seconds();
			Property->CopyCompleteValue(&Copied, Carrier.GetMemory());
			const double CopySeconds = FPlatformTime::Seconds() - Start;
			if (!Test.TestTrue(TEXT("timed preparation component copies every actual native field"), ComponentValueMatches(Copied))) return false;
			Start = FPlatformTime::Seconds();
			Carrier.Reset();
			const double ResetSeconds = FPlatformTime::Seconds() - Start;
			if (!Test.TestTrue(TEXT("timed reset ends carrier ownership while destination remains independent"),
				!Carrier.IsValid() && ComponentValueMatches(Copied))) return false;
			++SemanticSamples;
			// This separate real bridge observation is intentionally combined: Take resets before returning.
			FVaRestDynamicLifecycleRoot Taken;
			auto State = Capture(EVaRestTypeContainer::None);
			FVaRestDynamicBudgetLease Lease;
			if (!Test.TestTrue(TEXT("timed private Take owns a real state and admission"), State.IsValid() && Budget->TryReserve(Lease))) return false;
			bool bTaken = false, bConfirmed = false;
			double TakeSeconds = 0;
			if (!Test.TestTrue(TEXT("timed private Take binds the existing synchronous preparation"), State->SetPreparationOnGT([&](FDynamicPrepareToken& Token) {
				++PreparedSamples;
				PeakTrackedCarriers = FMath::Max(PeakTrackedCarriers, Testing::LiveDynamicCarriersOnGT());
				PeakReservedOutput = FMath::Max(PeakReservedOutput, Budget->OutputBytes());
				const double TakeStart = FPlatformTime::Seconds();
				bTaken = Token.Take(Property, &Taken);
				TakeSeconds = FPlatformTime::Seconds() - TakeStart;
				Test.TestEqual(TEXT("timed actual Take resets its tracked carrier"), Testing::LiveDynamicCarriersOnGT(), CarrierBaseline);
				Test.TestFalse(TEXT("timed actual Take cannot publish twice"), Token.Take(Property, &Taken));
				bConfirmed = Token.ConfirmPrepared();
				if (bConfirmed) ++ConfirmedSamples;
			}))) return false;
			FFinish Finish; FVaRestDynamicTypeInfo Response, ErrorInfo;
			State->FinalizeOnGT(Finish, Json, Response, ErrorInfo, Limits, Lease);
			Lease.Reset();
			if (!Test.TestTrue(TEXT("timed private transfer preserves actual success and complete values"),
				bTaken && bConfirmed && !Finish.Error.IsError() && Response.Availability == EVaRestDynamicAvailability::Available && ComponentValueMatches(Taken))) return false;
			if (!Test.TestTrue(TEXT("each timed admission releases all owned logical charges"),
				Budget->NumPending() == 0 && Budget->InputBytes() == 0 && Budget->ParseEstimateBytes() == 0 &&
				Budget->OutputBytes() == 0 && Testing::LiveDynamicCarriersOnGT() == CarrierBaseline)) return false;
			++ReleasedSamples;
			for (double Duration : {InitializeSeconds, ConvertSeconds, CopySeconds, ResetSeconds, TakeSeconds})
				if (!Test.TestTrue(TEXT("observed component duration is finite and nonnegative"), FMath::IsFinite(Duration) && Duration >= 0)) return false;
			if (Iteration >= 0)
			{
				Initialize.Add(InitializeSeconds); Convert.Add(ConvertSeconds); Copy.Add(CopySeconds);
				Reset.Add(ResetSeconds); TakeCombined.Add(TakeSeconds);
			}
		}
		CostReport = FVaRestJson::Object().Set(TEXT("schema"), TEXT("VaRest.M5.GTComponentCosts.1"))
			.Set(TEXT("test"), TEXT("VaRest.Http.Dynamic.OptionalMaterialization")).Set(TEXT("scope"), TEXT("qualified scalar struct with Optional leaf; component observations only"))
			.Set(TEXT("type"), Type->GetPathName()).Set(TEXT("struct_bytes"), Type->GetStructureSize()).Set(TEXT("destination_property_bytes"), Property->GetSize())
			.Set(TEXT("input"), Json).Set(TEXT("input_utf8_bytes"), Input.Num()).Set(TEXT("warmup_excluded"), Warmup).Set(TEXT("repetitions"), Repetitions)
			.Set(TEXT("timer"), TEXT("FPlatformTime::Seconds; seconds; single-operation wall intervals"))
			.Set(TEXT("timer_limits"), TEXT("uncalibrated near-resolution observations; scheduling and native fixture atomic-counter overhead remain; no exclusive-CPU or speed claim"))
			.Set(TEXT("initialize_as"), Samples(MoveTemp(Initialize))).Set(TEXT("json_to_struct"), Samples(MoveTemp(Convert)))
			.Set(TEXT("property_copy_complete_value"), Samples(MoveTemp(Copy))).Set(TEXT("carrier_reset"), Samples(MoveTemp(Reset)))
			.Set(TEXT("actual_private_take_copy_and_reset_combined"), Samples(MoveTemp(TakeCombined)))
			.Set(TEXT("semantic_samples_including_warmup"), SemanticSamples).Set(TEXT("actual_preparations"), PreparedSamples)
			.Set(TEXT("actual_confirmations"), ConfirmedSamples).Set(TEXT("released_admissions"), ReleasedSamples)
			.Set(TEXT("tracked_carrier_baseline"), CarrierBaseline).Set(TEXT("peak_tracked_carriers_observed"), PeakTrackedCarriers)
			.Set(TEXT("peak_logical_output_reservation_observed"), PeakReservedOutput).Set(TEXT("budget_pending_after"), Budget->NumPending())
			.Set(TEXT("budget_input_after"), Budget->InputBytes()).Set(TEXT("budget_parse_estimate_after"), Budget->ParseEstimateBytes()).Set(TEXT("budget_output_after"), Budget->OutputBytes())
			.Set(TEXT("test_success_external_required"), true).Set(TEXT("instrumented_pipeline_subspans"), false)
			.Set(TEXT("exclusions"), TEXT("not actual pipeline subspans; excludes parse/dispatch/rule-lookup/bag/container costs, setup/destination cleanup and report IO; no RSS/peak/allocator/default/caller bound or performance threshold; reused native fixture counters add overhead"));
		return true;
	}
	void WriteComponentReport()
	{
		check(IsInGameThread());
		if (!Test.TestTrue(TEXT("opt-in report owns completed finite component observations"), CostReport.IsObject())) return;
		// Root supplies a precreated owned directory. NoReplaceExisting refuses an old report.
		TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*CostReportPath, FILEWRITE_NoReplaceExisting));
		if (!Test.TestTrue(TEXT("opt-in report creates a fresh owned file"), Writer.IsValid())) return;
		TArray<uint8> Bytes; CostReport.ToUtf8(Bytes);
		Writer->Serialize(Bytes.GetData(), Bytes.Num());
		const bool bWriteOk = !Writer->IsError();
		const bool bClosed = Writer->Close();
		Test.TestTrue(TEXT("opt-in machine-readable report writes and closes"), bWriteOk && bClosed);
	}
	void Advance() { PublicationFrame = GFrameCounter; ++Phase; }
	void Cleanup()
	{
		check(IsInGameThread()); Destination.Reset();
		if (bRoot) { FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestDynamicLifecycleRoot::StaticStruct()); bRoot = false; }
		if (bLeaf) { FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestDynamicLifecycleLeaf::StaticStruct()); bLeaf = false; }
		if (bObserving) { EndObservationOnGT(); bObserving = false; }
	}
	FAutomationTestBase& Test;
	const double Deadline;
	const int32 CarrierBaseline;
	FString Error;
	TUniquePtr<FVaRestDynamicLifecycleDestinations> Destination;
	uint64 PublicationFrame = 0;
	int32 Phase = 0;
	bool bRoot = false, bLeaf = false, bObserving = false;
	const bool bMeasureComponents = FParse::Param(FCommandLine::Get(), TEXT("VaRestM5GTComponentCosts"));
	FString CostReportPath;
	FVaRestJson CostReport;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestDynamicOptionalLifecycleTest, "VaRest.Http.Dynamic.OptionalMaterialization",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestDynamicOptionalLifecycleTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(VaRest::Http::DynamicLifecycleTest::FQualification(*this));
	return true;
}
#endif
