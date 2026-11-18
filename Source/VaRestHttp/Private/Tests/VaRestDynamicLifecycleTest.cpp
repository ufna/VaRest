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
			Advance(); return false;
		}
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
