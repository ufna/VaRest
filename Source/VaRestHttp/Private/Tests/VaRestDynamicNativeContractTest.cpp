// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "VaRestDynamicClosure.h"
#include "VaRestDynamicTypes.h"
#include "VaRestHttpBlueprintTypes.h"
#include "VaRestHttpTesting.h"
#include "CoreGlobals.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "UObject/Class.h"

namespace VaRest::Http::DynamicNativeContractTest
{
// These are real reflected plugin types, not a replacement registry or a simulated frame clock.
const FName RootContract(TEXT("VaRest.Test.DynamicGT.Response.v1"));
const FName HeaderContract(TEXT("VaRest.Test.DynamicGT.Header.v1"));
const FName IssueContract(TEXT("VaRest.Test.DynamicGT.Issue.v1"));
const FName DependencyContract(TEXT("VaRest.Test.DynamicGT.Dependency.v1"));

class FQualification final : public IAutomationLatentCommand
{
public:
	explicit FQualification(FAutomationTestBase& InTest)
		: Test(InTest), Deadline(FPlatformTime::Seconds() + 15.0), CarrierBaseline(Testing::LiveDynamicCarriersOnGT()) {}

	~FQualification() override { Cleanup(); }

	bool Update() override
	{
		check(IsInGameThread());
		if (FPlatformTime::Seconds() >= Deadline)
		{
			Test.AddError(TEXT("native contract qualification did not settle on actual engine frames"));
			Cleanup();
			return true;
		}
		if (Phase == 0)
		{
			auto Control = Intrinsic();
			if (!Control) return false; // a previous case may still be settling through a real EndFrame
			Test.TestTrue(TEXT("real GT runtime and editor hooks admit the intrinsic control"), Control.IsValid());
			Refused(TEXT("unregistered real native root is refused"));
			const UObject* Dependencies[] = {FVaRestQueryParameter::StaticStruct()};
			bRoot = FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestResponseData::StaticStruct(), RootContract, Dependencies);
			bHeader = FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestHeader::StaticStruct(), HeaderContract);
			bIssue = FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestDecodeIssue::StaticStruct(), IssueContract);
			if (!Test.TestTrue(TEXT("register real root and both reflected leaf contracts"), bRoot && bHeader && bIssue))
			{
				Cleanup();
				return true;
			}
			Test.TestFalse(TEXT("registration invalidates an already captured schema"), Control->ValidateActivationOnGT(Error));
			Advance();
			return false;
		}
		// Do not change GFrameCounter or broadcast a fake EndFrame. Admission must settle through the real runtime.
		if (GFrameCounter <= PublicationFrame || !Intrinsic().IsValid()) return false;
		switch (Phase)
		{
		case 1:
			// Both reflected leaves are qualified. The explicit native dependency alone is still unqualified.
			Refused(TEXT("an unqualified declared native dependency prevents root admission"));
			bDependency = FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestQueryParameter::StaticStruct(), DependencyContract);
			if (!Test.TestTrue(TEXT("register the real declared native dependency"), bDependency)) { Cleanup(); return true; }
			Advance();
			return false;
		case 2:
			Scalar = Capture(EVaRestTypeContainer::None);
			Array = Capture(EVaRestTypeContainer::Array);
			Map = Capture(EVaRestTypeContainer::Map);
			if (!Test.TestTrue(TEXT("complete real native closure admits scalar, array and string-key map"), Scalar.IsValid() && Array.IsValid() && Map.IsValid()))
			{
				Cleanup();
				return true;
			}
			{
				const UObject* Dependencies[] = {FVaRestQueryParameter::StaticStruct()};
				Test.TestTrue(TEXT("same contract and dependencies are idempotent"),
					FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestResponseData::StaticStruct(), RootContract, Dependencies));
				Test.TestFalse(TEXT("a different contract cannot replace the captured declaration"),
					FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestResponseData::StaticStruct(), TEXT("VaRest.Test.DynamicGT.Conflicting.v1"), Dependencies));
			}
			Test.TestTrue(TEXT("idempotent/conflicting registration preserves the current capture"), Scalar->ValidateActivationOnGT(Error));
			FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestDecodeIssue::StaticStruct());
			bIssue = false;
			Advance();
			return false;
		case 3:
			Test.TestFalse(TEXT("revoked nested leaf invalidates old scalar capture after real settling"), Scalar->ValidateActivationOnGT(Error));
			Test.TestFalse(TEXT("revoked nested leaf invalidates old array capture"), Array->ValidateActivationOnGT(Error));
			Test.TestFalse(TEXT("revoked nested leaf invalidates old map capture"), Map->ValidateActivationOnGT(Error));
			Refused(TEXT("a native struct nested in the real DecodeWarnings array must remain qualified"));
			bIssue = FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestDecodeIssue::StaticStruct(), IssueContract);
			if (!Test.TestTrue(TEXT("restore the actual nested leaf declaration"), bIssue)) { Cleanup(); return true; }
			Advance();
			return false;
		case 4:
			Restored = Capture(EVaRestTypeContainer::None);
			if (!Test.TestTrue(TEXT("restored full closure admits a new capture"), Restored.IsValid())) { Cleanup(); return true; }
			Test.TestFalse(TEXT("restoration does not revive an old schema capture"), Scalar->ValidateActivationOnGT(Error));
			FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestQueryParameter::StaticStruct());
			bDependency = false;
			Advance();
			return false;
		case 5:
			Test.TestFalse(TEXT("native dependency revocation invalidates the restored capture"), Restored->ValidateActivationOnGT(Error));
			Refused(TEXT("revoked explicit native dependency cannot be ignored even with qualified reflected fields"));
			Cleanup();
			Advance();
			return false;
		default:
			Test.TestEqual(TEXT("admission/revocation creates no live reflected value carriers"), Testing::LiveDynamicCarriersOnGT(), CarrierBaseline);
			// The intrinsic control above proves cleanup has settled before the next automation case.
			return true;
		}
	}

private:
	TSharedPtr<FDynamicTypedState, ESPMode::ThreadSafe> Capture(EVaRestTypeContainer Container)
	{
		FVaRestDynamicResultConfig Config;
		Config.ResponseType.Kind = EVaRestTypeKind::Struct;
		Config.ResponseType.Type = FVaRestResponseData::StaticStruct();
		Config.ResponseType.Container = Container;
		return FDynamicTypedState::CreateOnGT(Config, Error);
	}

	TSharedPtr<FDynamicTypedState, ESPMode::ThreadSafe> Intrinsic()
	{
		FVaRestDynamicResultConfig Config;
		Config.ResponseType.Kind = EVaRestTypeKind::Struct;
		Config.ResponseType.Type = FVaRestJson::StaticStruct();
		return FDynamicTypedState::CreateOnGT(Config, Error);
	}

	void Refused(const TCHAR* Message)
	{
		Error.Reset();
		Test.TestFalse(Message, Capture(EVaRestTypeContainer::None).IsValid());
		Test.TestTrue(TEXT("refusal identifies the actual native lifecycle contract"), Error.Contains(TEXT("qualified game-thread lifecycle contract")));
		Test.TestEqual(TEXT("refusal happens before reflected value allocation"), Testing::LiveDynamicCarriersOnGT(), CarrierBaseline);
	}

	void Advance() { PublicationFrame = GFrameCounter; ++Phase; }

	void Cleanup()
	{
		check(IsInGameThread());
		Scalar.Reset(); Array.Reset(); Map.Reset(); Restored.Reset();
		if (bRoot) { FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestResponseData::StaticStruct()); bRoot = false; }
		if (bHeader) { FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestHeader::StaticStruct()); bHeader = false; }
		if (bIssue) { FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestDecodeIssue::StaticStruct()); bIssue = false; }
		if (bDependency) { FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestQueryParameter::StaticStruct()); bDependency = false; }
	}

	FAutomationTestBase& Test;
	const double Deadline;
	const int32 CarrierBaseline;
	FString Error;
	TSharedPtr<FDynamicTypedState, ESPMode::ThreadSafe> Scalar, Array, Map, Restored;
	uint64 PublicationFrame = 0;
	int32 Phase = 0;
	bool bRoot = false, bHeader = false, bIssue = false, bDependency = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestDynamicNativeContractTest, "VaRest.Http.Dynamic.NativeContracts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestDynamicNativeContractTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(VaRest::Http::DynamicNativeContractTest::FQualification(*this));
	return true;
}

#endif
