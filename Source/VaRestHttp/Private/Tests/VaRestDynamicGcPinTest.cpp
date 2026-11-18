// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "VaRestDynamicClosure.h"
#include "VaRestHttpTesting.h"
#include "CoreGlobals.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace VaRest::Http::Testing::GcPinPrivate
{
using FState = TSharedPtr<FDynamicTypedState, ESPMode::ThreadSafe>;

struct FFixture
{
    TStrongObjectPtr<UEnum> Enum;
    TStrongObjectPtr<UObject> Owner;
    TArray<FState> States;
    TArray<TWeakPtr<FDynamicTypedState, ESPMode::ThreadSafe>> WeakStates;
    TWeakObjectPtr<UEnum> WeakEnum;
    TWeakObjectPtr<UObject> WeakOwner;

    bool CreateObjects(FAutomationTestBase& Test)
    {
        const FName EnumName = MakeUniqueObjectName(GetTransientPackage(), UEnum::StaticClass(), FName(TEXT("VaRestDynamicGcEnum")));
        Enum.Reset(NewObject<UEnum>(GetTransientPackage(), EnumName, RF_Transient));
        Owner.Reset(NewObject<UObject>(GetTransientPackage(), NAME_None, RF_Transient));
        if (!Test.TestNotNull(TEXT("owned real enum"), Enum.Get()) || !Test.TestNotNull(TEXT("owned rule owner"), Owner.Get())) return false;
        TArray<TPair<FName, int64>> Values;
        Values.Emplace(FName(*(EnumName.ToString() + TEXT("::Entry"))), int64(0));
        if (!Test.TestTrue(TEXT("real runtime enum definitions installed"), Enum->SetEnums(Values, UEnum::ECppForm::EnumClass,
            UEnum::EUnderlyingType::uint8, EEnumFlags::None, UEnum::EAddMaxKeyIfMissing::Yes))) return false;
        WeakEnum = Enum.Get(); WeakOwner = Owner.Get();
        return true;
    }

    bool CaptureStates(FAutomationTestBase& Test)
    {
        for (const auto Container : {EVaRestTypeContainer::None, EVaRestTypeContainer::Array, EVaRestTypeContainer::Map})
        {
            FVaRestDynamicResultConfig Config;
            Config.ResponseType.Kind = EVaRestTypeKind::Enum;
            Config.ResponseType.Type = Enum.Get();
            Config.ResponseType.Container = Container;
            Config.RuleOwner = Owner.Get();
            FString Error, Code;
            auto State = FDynamicTypedState::CreateOnGT(Config, Error, &Code);
            if (!Test.TestTrue(TEXT("actual scalar/array/map GT state captured"), State.IsValid()))
            {
                Test.AddError(Code + TEXT(": ") + Error);
                return false;
            }
            Test.TestTrue(TEXT("actual admitted output is typed"), State->HasTypedResponseOnGT());
            WeakStates.Add(State);
            States.Add(MoveTemp(State));
            // Config is gone before either GC. It is never an external root for the test.
        }
        return true;
    }

    void DropExternalOwners()
    {
        Enum.Reset(); Owner.Reset();
    }
};

class FRun final : public IAutomationLatentCommand
{
public:
    explicit FRun(FAutomationTestBase& InTest) : Test(InTest), Started(FPlatformTime::Seconds()) {}

    virtual bool Update() override
    {
        check(IsInGameThread());
        if (FPlatformTime::Seconds() - Started > 30)
        {
            Test.AddError(TEXT("actual GT GC state-pin qualification timed out"));
            return true;
        }
        if (Stage == 0)
        {
            CarrierBaseline = VaRest::Http::Testing::LiveDynamicCarriersOnGT();
            if (!Live.CreateObjects(Test) || !Dropped.CreateObjects(Test)) return true;
            BuiltFrame = GFrameCounter;
            Stage = 1;
            return false;
        }
        if (Stage == 1)
        {
            // Existing editor hooks must settle on real engine frames; never fabricate hooks or frames.
            if (GFrameCounter <= BuiltFrame + 1) return false;
            if (!Live.CaptureStates(Test) || !Dropped.CaptureStates(Test)) return true;
            Live.DropExternalOwners(); Dropped.DropExternalOwners();
            Dropped.States.Reset();
            for (const auto& Weak : Dropped.WeakStates)
                Test.TestFalse(TEXT("matching dropped-state control has no native owner"), Weak.IsValid());
            Test.TestTrue(TEXT("matching control enum exists before actual GC"), Dropped.WeakEnum.IsValid());
            Test.TestTrue(TEXT("matching control owner exists before actual GC"), Dropped.WeakOwner.IsValid());
            CollectGarbage(RF_NoFlags, true);
            Test.TestFalse(TEXT("full GC actually collects matching unowned enum"), Dropped.WeakEnum.IsValid());
            Test.TestFalse(TEXT("full GC actually collects matching unowned rule owner"), Dropped.WeakOwner.IsValid());
            Test.TestTrue(TEXT("live state/bag closure retains actual enum through full GC"), Live.WeakEnum.IsValid());
            Test.TestTrue(TEXT("live state pin retains actual rule owner through full GC"), Live.WeakOwner.IsValid());
            for (const auto& State : Live.States)
            {
                FString Error;
                Test.TestTrue(TEXT("GC-preserved state remains admissible"), State->ValidateActivationOnGT(Error));
            }
            Stage = 2;
            return false;
        }
        for (const auto& State : Live.States)
        {
            State->InvalidateOnGT(); // Only this fixture's states; no global invalidation/registry reset.
            FString Error;
            Test.TestFalse(TEXT("owned invalidation refuses later activation"), State->ValidateActivationOnGT(Error));
        }
        CollectGarbage(RF_NoFlags, true);
        Test.TestFalse(TEXT("invalidated states no longer pin their actual enum or bag type"), Live.WeakEnum.IsValid());
        Test.TestFalse(TEXT("invalidated states no longer pin their actual rule owner"), Live.WeakOwner.IsValid());
        for (const auto& State : Live.States)
        {
            Test.TestTrue(TEXT("stale native state shell remains held during GC retirement"), State.IsValid());
            FString Error;
            Test.TestFalse(TEXT("stale shell rejects activation after owned objects were GC'd"), State->ValidateActivationOnGT(Error));
        }
        // Never dereference a dead weak type/owner. Native stale shells own no reflected storage.
        Live.States.Reset();
        for (const auto& Weak : Live.WeakStates)
            Test.TestFalse(TEXT("runtime weak state tracking retains no dropped native shell"), Weak.IsValid());
        Test.TestEqual(TEXT("capture and GC qualification creates no materialized carrier"),
            VaRest::Http::Testing::LiveDynamicCarriersOnGT(), CarrierBaseline);
        return true;
    }

private:
    FAutomationTestBase& Test;
    FFixture Live, Dropped;
    double Started;
    uint64 BuiltFrame = 0;
    int32 CarrierBaseline = 0;
    uint8 Stage = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestDynamicGcPinsTest, "VaRest.Http.Dynamic.GcStatePins",
    (WITH_EDITOR ? EAutomationTestFlags::EditorContext : (EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext)) | EAutomationTestFlags::EngineFilter)

bool FVaRestDynamicGcPinsTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(VaRest::Http::Testing::GcPinPrivate::FRun(*this));
    return true;
}

#endif
