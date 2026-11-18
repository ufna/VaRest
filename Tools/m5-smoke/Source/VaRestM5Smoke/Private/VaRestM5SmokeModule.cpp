// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestM5SettingsOnlyProof.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameEngine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProperties.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"
#include "VaRestApi.h"
#include "VaRestEngineSubsystem.h"
#include "VaRestHttpSettings.h"
#include "VaRestTrace.h"

DEFINE_LOG_CATEGORY_STATIC(LogVaRestM5SettingsOnly, Log, All);
namespace
{
class FEarlyRequestWitness final : public IVaRestTraceSink
{
public:
    int32 Started = 0;
    int32 AttachedInFlight = 0;
    virtual FVaRestTraceDetailSettings GetDetail() const override
    {
        FVaRestTraceDetailSettings Result; Result.Flags = EVaRestTraceDetail::Metadata; return Result;
    }
    virtual void OnAttached(const FVaRestTraceSnapshot& Snapshot) override { AttachedInFlight = Snapshot.InFlight.Num(); }
    virtual void OnEvent(const FVaRestTraceEvent& Event) override
    {
        if (Event.Kind == EVaRestTraceKind::Started) { check(IsInGameThread()); ++Started; }
    }
};
}

class FVaRestM5SmokeModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        if (!FParse::Param(FCommandLine::Get(), TEXT("VaRestM5SettingsOnly"))) return;
        if (IsRunningCommandlet())
        {
            UE_LOG(LogVaRestM5SettingsOnly, Display, TEXT("VAREST_M5_SETTINGS_NOT_RUN context=Commandlet; no gameplay preload or smoke conversion"));
            return;
        }
        if (GIsEditor || !FPlatformProperties::RequiresCookedData() || GEngine
            || FindObject<UObject>(nullptr, VaRestM5SettingsOnly::ApiPath)
            || FindObject<UObject>(nullptr, VaRestM5SettingsOnly::RootPath)
            || FindObject<UObject>(nullptr, VaRestM5SettingsOnly::NestedPath)
            || FindObject<UObject>(nullptr, VaRestM5SettingsOnly::EnumPath))
        {
            Fail(TEXT("Entry requires a fresh cooked Game process, Default-phase arming before GEngine, and B initially absent"));
            return;
        }
        Witness = MakeShared<FEarlyRequestWitness, ESPMode::ThreadSafe>();
        FVaRestTrace::AddSink(Witness.ToSharedRef());
        PostInit = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FVaRestM5SmokeModule::ProveBeforeStart);
        UE_LOG(LogVaRestM5SettingsOnly, Display, TEXT("VAREST_M5_SETTINGS_ARMED api=absent root=absent nested=absent enum=absent phase=Default context=CookedGame"));
    }
    virtual void ShutdownModule() override
    {
        FCoreDelegates::GetOnPostEngineInit().Remove(PostInit);
        DropWitness();
    }
    virtual bool SupportsDynamicReloading() override { return false; }
private:
    void DropWitness()
    {
        if (Witness) { FVaRestTrace::RemoveSink(Witness.ToSharedRef()); Witness.Reset(); }
    }
    void Fail(const TCHAR* Reason)
    {
        UE_LOG(LogVaRestM5SettingsOnly, Error, TEXT("VAREST_M5_SETTINGS_FAIL reason=%s"), Reason);
        FPlatformMisc::RequestExitWithStatus(false, 71, TEXT("VaRestM5SettingsOnly"));
    }
    void ProveBeforeStart()
    {
        using namespace VaRestM5SettingsOnly;
        FCoreDelegates::GetOnPostEngineInit().Remove(PostInit); PostInit.Reset();
        check(IsInGameThread());
        if (!GEngine || !Cast<UGameEngine>(GEngine) || IsRunningCommandlet() || !Witness
            || Witness->Started != 0 || Witness->AttachedInFlight != 0)
        { DropWitness(); Fail(TEXT("Not the normal running Game hook or a request preceded first conversion")); return; }
        // UGameEngine::Init creates the stock GI before this hook; reject user GI startup code as a confounder.
        int32 OrdinaryGameInstances = 0;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.World() && Context.World()->HasBegunPlay())
            { DropWitness(); Fail(TEXT("World began play before proof; ordering is invalid")); return; }
            if (Context.OwningGameInstance)
            {
                if (Context.OwningGameInstance->GetClass() != UGameInstance::StaticClass())
                { DropWitness(); Fail(TEXT("B proof requires the ordinary stock GameInstance, no custom startup loader")); return; }
                ++OrdinaryGameInstances;
            }
        }
        if (OrdinaryGameInstances != 1)
        { DropWitness(); Fail(TEXT("Expected exactly one ordinary stock GameInstance")); return; }
        // FindObject never loads a package. The production subsystem must already strongly retain exactly this API.
        UVaRestApi* Api = FindObject<UVaRestApi>(nullptr, ApiPath);
        UScriptStruct* Root = FindObject<UScriptStruct>(nullptr, RootPath);
        UObject* Nested = FindObject<UObject>(nullptr, NestedPath);
        UObject* Enum = FindObject<UObject>(nullptr, EnumPath);
        const UVaRestEngineSubsystem* Loader = GEngine->GetEngineSubsystem<UVaRestEngineSubsystem>();
        int32 ConfiguredB = 0;
        for (const auto& Entry : GetDefault<UVaRestHttpSettings>()->Apis)
            if (Entry.ToSoftObjectPath().ToString().Equals(ApiPath, ESearchCase::CaseSensitive)) ++ConfiguredB;
        if (!Api || !Root || !Nested || !Enum || !Loader || ConfiguredB != 1 || !Loader->GetLoadedApis().Contains(Api))
        { DropWitness(); Fail(TEXT("Production preload/ModifyCook failed: settings-only API/types absent from no-load lookup or production owner")); return; }
        UE_LOG(LogVaRestM5SettingsOnly, Display, TEXT("VAREST_M5_SETTINGS_BEFORE_CONVERSION hook=GetOnPostEngineInit before=GEngineStart loaderOwnsB=1 configuredB=1 startedRequests=0 lookup=FindObject"));
        FString Error;
        const bool bPassed = VerifyDefaultConversion(Api, Root, Error);
        const bool bNoRequests = Witness->Started == 0;
        DropWitness();
        if (!bPassed || !bNoRequests)
        { Fail(bPassed ? TEXT("Request began during standalone proof") : *Error); return; }
        UE_LOG(LogVaRestM5SettingsOnly, Display, TEXT("VAREST_M5_SETTINGS_PASS proof=B noLoadLookup=1 productionOwner=1 defaultOptions=1 before=GEngineStart startedRequests=0 mode=GameThread; proofA=OPEN wholeTask14=OPEN"));
        if (FParse::Param(FCommandLine::Get(), TEXT("VaRestM5SettingsExit")))
            FPlatformMisc::RequestExitWithStatus(false, 0, TEXT("VaRestM5SettingsOnly"));
    }
    FDelegateHandle PostInit;
    TSharedPtr<FEarlyRequestWitness, ESPMode::ThreadSafe> Witness;
};
IMPLEMENT_MODULE(FVaRestM5SmokeModule, VaRestM5Smoke);
