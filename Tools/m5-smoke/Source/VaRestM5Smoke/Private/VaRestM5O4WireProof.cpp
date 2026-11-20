// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestM5O4WireProof.h"
#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameEngine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProperties.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/EnumProperty.h"
#include "UObject/StructOnScope.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "VaRestApi.h"
#include "VaRestClient.h"
#include "VaRestEngineSubsystem.h"
#include "VaRestHttpSettings.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonRules.h"
#include "VaRestTrace.h"

DEFINE_LOG_CATEGORY_STATIC(LogVaRestM5O4, Log, All);
namespace VaRestM5O4
{
namespace
{
const FVaRestJsonFieldRule* RuleFor(const UVaRestApi* Api, const UScriptStruct* Root, const TCHAR* Wire)
{
    for (const auto& Type : Api->JsonTypes)
        if (Type.Type.Get() == Root)
            for (const auto& Rule : Type.Fields)
                if (Rule.Json.Equals(Wire, ESearchCase::CaseSensitive)) return &Rule;
    return nullptr;
}
bool Golden(const FVaRestJson& Json, const TCHAR* Pretty)
{
    return Json.IsObject() && Json.Num() == 4 && Json[TEXT("Wire.Score")].IsNumber()
        && Json[TEXT("Wire.Score")].AsInt() == 7 && Json[TEXT("@type")].AsString() == TEXT("alpha")
        && Json[TEXT("nullCount")].IsNull() && Json[TEXT("pretty")].AsString() == Pretty
        && !Json[TEXT("omitCount")].IsValid() && !Json[TEXT("state")].IsValid();
}
class FEarlyWitness final : public IVaRestTraceSink
{
public:
    int32 Started = 0;
    int32 Attached = 0;
    virtual FVaRestTraceDetailSettings GetDetail() const override
    {
        FVaRestTraceDetailSettings Result; Result.Flags = EVaRestTraceDetail::Metadata; return Result;
    }
    virtual void OnAttached(const FVaRestTraceSnapshot& Snapshot) override { Attached = Snapshot.InFlight.Num(); }
    virtual void OnEvent(const FVaRestTraceEvent& Event) override
    {
        if (Event.Kind == EVaRestTraceKind::Started) { check(IsInGameThread()); ++Started; }
    }
};
class FProof final : public TSharedFromThis<FProof, ESPMode::ThreadSafe>
{
public:
    explicit FProof(bool InCooked) : bCooked(InCooked) {}
    void Arm()
    {
        check(IsInGameThread());
        Outcome = 0;
        if (FParse::Param(FCommandLine::Get(), TEXT("VaRestO4WireCooked")) == FParse::Param(FCommandLine::Get(), TEXT("VaRestO4WireEditor"))
            || IsRunningCommandlet() || GEngine || (bCooked ? (GIsEditor || !FPlatformProperties::RequiresCookedData()) : !GIsEditor)
            || FindObject<UObject>(nullptr, ApiPath))
        { Finish(false, TEXT("Requires opt-in Default-phase arming before GEngine/API, in the declared Editor or cooked context")); return; }
        if (!FParse::Value(FCommandLine::Get(), TEXT("VaRestO4Proof="), ProofFile)
            || FPaths::IsRelative(ProofFile) || IFileManager::Get().FileExists(*ProofFile)
            || !IFileManager::Get().DirectoryExists(*FPaths::GetPath(ProofFile)))
        { Finish(false, TEXT("Root must supply a fresh absolute proof file in an existing owned output directory")); return; }
        Witness = MakeShared<FEarlyWitness, ESPMode::ThreadSafe>();
        FVaRestTrace::AddSink(Witness.ToSharedRef());
        const TWeakPtr<FProof, ESPMode::ThreadSafe> Weak = AsShared();
        PostInit = FCoreDelegates::GetOnPostEngineInit().AddLambda([Weak]()
        {
            if (const auto Self = Weak.Pin()) Self->AfterPreload();
        });
        UE_LOG(LogVaRestM5O4, Display, TEXT("VAREST_O4_ARMED phase=Default apiAbsent=1 beforeGEngine=1 cooked=%d"), bCooked ? 1 : 0);
    }
    void Shutdown()
    {
        check(IsInGameThread());
        FCoreDelegates::GetOnPostEngineInit().Remove(PostInit); PostInit.Reset();
        FTSTicker::RemoveTicker(TickHandle); TickHandle.Reset();
        DropWitness();
        Request.Cancel();
    }
    int32 Outcome = -1;
    FString Detail;
    FString Wire;
private:
    void DropWitness()
    {
        if (Witness) { FVaRestTrace::RemoveSink(Witness.ToSharedRef()); Witness.Reset(); }
    }
    void Finish(bool bSuccess, const TCHAR* Reason)
    {
        check(IsInGameThread());
        if (Outcome != 0) return;
        Outcome = bSuccess ? 1 : 2;
        Detail = Reason;
        DropWitness();
        if (!bSuccess) Request.Cancel();
        auto Result = FVaRestJson::Object().Set(TEXT("schema"), TEXT("VaRest.O4.ImportedWire.1"))
            .Set(TEXT("passed"), bSuccess).Set(TEXT("cooked"), bCooked).Set(TEXT("api"), ApiPath)
            .Set(TEXT("early_conversion"), bEarlyConversion).Set(TEXT("wire"), Wire)
            .Set(TEXT("echo_body"), EchoBody).Set(TEXT("status"), Status).Set(TEXT("callback_count"), CallbackCount)
            .Set(TEXT("reason"), Detail);
        // The caller owns this new path. Never replace a prior proof, even on a failed fixture.
        const bool bSaved = !ProofFile.IsEmpty() && !IFileManager::Get().FileExists(*ProofFile)
            && FFileHelper::SaveStringToFile(Result.ToString(), *ProofFile,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_NoReplaceExisting);
        if (!bSaved) { Outcome = 2; Detail += TEXT("; fresh proof write failed"); }
        UE_LOG(LogVaRestM5O4, Display, TEXT("VAREST_O4_TERMINAL pass=%d early=%d callbacks=%d status=%d reason=%s"),
            Outcome == 1 ? 1 : 0, bEarlyConversion ? 1 : 0, CallbackCount, Status, *Detail);
        if (bCooked) FPlatformMisc::RequestExitWithStatus(false, Outcome == 1 ? 0 : 72, TEXT("VaRestO4WireCooked"));
    }
    void AfterPreload()
    {
        FCoreDelegates::GetOnPostEngineInit().Remove(PostInit); PostInit.Reset();
        if (Outcome != 0) return;
        if (!GEngine || (bCooked && !Cast<UGameEngine>(GEngine)) || !Witness || Witness->Started || Witness->Attached)
        { Finish(false, TEXT("Engine/startup request witness differs")); return; }
        int32 Instances = 0;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.World() && Context.World()->HasBegunPlay())
            { Finish(false, TEXT("Conversion was armed after world BeginPlay")); return; }
            if (UGameInstance* Instance = Context.OwningGameInstance)
            {
                if (Instance->GetClass() != UGameInstance::StaticClass())
                { Finish(false, TEXT("Fixture requires the ordinary stock GameInstance")); return; }
                ++Instances;
            }
        }
        if (bCooked && Instances != 1)
        { Finish(false, TEXT("Cooked startup must expose exactly one stock GameInstance")); return; }
        // Find only. Settings/production EngineSubsystem must already own the asset and its imported type rules.
        Api.Reset(FindObject<UVaRestApi>(nullptr, ApiPath));
        auto* Loader = GEngine->GetEngineSubsystem<UVaRestEngineSubsystem>();
        int32 Configured = 0;
        for (const auto& Entry : GetDefault<UVaRestHttpSettings>()->Apis)
            if (Entry.ToSoftObjectPath().ToString().Equals(ApiPath, ESearchCase::CaseSensitive)) ++Configured;
        auto* Root = Api.IsValid() && Api->Endpoints.Num() == 1
            ? Cast<UScriptStruct>(Api->Endpoints[0].ResponseType.Type.Get()) : nullptr;
        if (!Api.IsValid() || !Loader || !Loader->GetLoadedApis().Contains(Api.Get()) || Configured != 1
            || GetDefault<UVaRestHttpSettings>()->Apis.Num() != 1 || !Root || Api->Environments.Num() != 1
            || !Api->Environments[0].BaseUrl.Equals(BaseUrl, ESearchCase::CaseSensitive))
        { Finish(false, TEXT("Configured API was not preloaded with its actual imported root and loopback environment")); return; }
        FString Error;
        // First conversion: default options, no explicit owner snapshot and no CallEndpoint or asset load.
        if (!VerifyConversion(Api.Get(), Root, Wire, Error) || Witness->Started || Witness->Attached)
        { Finish(false, Error.IsEmpty() ? TEXT("A request preceded conversion") : *Error); return; }
        bEarlyConversion = true;
        DropWitness();
        UE_LOG(LogVaRestM5O4, Display, TEXT("VAREST_O4_EARLY apiNoLoad=1 rulesFromStartup=1 beforeBeginPlay=1 started=0 wire=%s"), *Wire);
        Client = FVaRestClient::Make(Api->Environments[0].BaseUrl);
        Client.WithTimeout(20.f, 10.f);
        Deadline = FPlatformTime::Seconds() + 30.;
        const TWeakPtr<FProof, ESPMode::ThreadSafe> Weak = AsShared();
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(TEXT("VaRestO4WireDeadline"), 0.f, [Weak](float)
        {
            const auto Self = Weak.Pin();
            if (!Self || Self->Outcome != 0) return false;
            if (FPlatformTime::Seconds() > Self->Deadline)
            { Self->Finish(false, TEXT("Owned O4 request deadline expired")); return false; }
            return true;
        });
        Request = Client.Post<FVaRestJson>(EchoPath).Body(Wire).Header(TEXT("Content-Type"), TEXT("application/json"))
            .OnComplete([Weak](const TVaRestResult<FVaRestJson>& Response)
            {
                const auto Self = Weak.Pin();
                if (!Self || Self->Outcome != 0) return;
                check(IsInGameThread());
                ++Self->CallbackCount;
                Self->Status = Response.Response.Status;
                if (!Response.IsSuccess() || !Response.Value.IsSet() || Self->Status != 200 || Self->CallbackCount != 1)
                { Self->Finish(false, TEXT("Ordinary public client did not complete exactly once with HTTP 200")); return; }
                const FVaRestJson Echo = Response.GetValue();
                const FTCHARToUTF8 Utf8(*Self->Wire, Self->Wire.Len());
                Self->EchoBody = Echo[TEXT("body")].AsString();
                if (Echo[TEXT("method")].AsString() != TEXT("POST") || Echo[TEXT("path")].AsString() != EchoPath
                    || Echo[TEXT("content_type")].AsString() != TEXT("application/json")
                    || Echo[TEXT("body_length")].AsInt() != Utf8.Length()
                    || !Self->EchoBody.Equals(Self->Wire, ESearchCase::CaseSensitive))
                { Self->Finish(false, TEXT("Official echo raw body, byte length, method, path or content type differs")); return; }
                auto* EchoRoot = Cast<UScriptStruct>(Self->Api->Endpoints[0].ResponseType.Type.Get());
                const auto Parsed = FVaRestJson::Parse(FStringView(Self->EchoBody));
                FStructOnScope Decoded(EchoRoot);
                FVaRestJsonDecodeReport Report;
                if (!Parsed.HasValue() || !FVaRestJsonConvert::JsonToStruct(Parsed.GetValue(), EchoRoot, Decoded.GetStructMemory(),
                        FVaRestJsonConvertOptions(), &Report) || Report.Errors.Num() || Report.Warnings.Num() || Report.bBudgetExceeded
                    || !FVaRestJsonConvert::StructToString(EchoRoot, Decoded.GetStructMemory()).Equals(Self->Wire, ESearchCase::CaseSensitive))
                { Self->Finish(false, TEXT("Actual echoed wire does not decode/reencode byte-identically with default rules")); return; }
                Self->Finish(true, TEXT("Imported six-feature struct default encode/decode and official echo passed"));
            }).Send();
        if (!Request.GetId()) Finish(false, TEXT("Public request handle was not admitted"));
    }
    bool bCooked = false;
    bool bEarlyConversion = false;
    int32 CallbackCount = 0;
    int32 Status = 0;
    double Deadline = 0.;
    FString ProofFile;
    FString EchoBody;
    TStrongObjectPtr<UVaRestApi> Api{ nullptr };
    FVaRestClient Client;
    FVaRestRequestHandle Request;
    TSharedPtr<FEarlyWitness, ESPMode::ThreadSafe> Witness;
    FDelegateHandle PostInit;
    FTSTicker::FDelegateHandle TickHandle;
};
TSharedPtr<FProof, ESPMode::ThreadSafe> Proof;
}

bool VerifyConversion(UVaRestApi* Api, UScriptStruct* Root, FString& Wire, FString& Error)
{
    check(IsInGameThread());
    const auto Fail = [&Error](const TCHAR* Why) { Error = Why; return false; };
    if (!Api || !Root || !Api->GetPathName().Equals(ApiPath, ESearchCase::CaseSensitive)
        || !Api->TypesFolder.Equals(TypesFolder, ESearchCase::CaseSensitive)
        || !Root->GetPathName().StartsWith(FString(TypesFolder) + TEXT("/"), ESearchCase::CaseSensitive)
        || !Root->ArePropertyGuidsAvailable() || FVaRestJsonRules::GetGeneration(Api) == 0)
        return Fail(TEXT("Actual imported root/GUIDs/registered rules are unavailable"));
    const auto* Score = RuleFor(Api, Root, TEXT("Wire.Score"));
    const auto* Type = RuleFor(Api, Root, TEXT("@type"));
    const auto* Omit = RuleFor(Api, Root, TEXT("omitCount"));
    const auto* Null = RuleFor(Api, Root, TEXT("nullCount"));
    const auto* State = RuleFor(Api, Root, TEXT("state"));
    const auto* Pretty = RuleFor(Api, Root, TEXT("pretty"));
    if (!Score || !Type || !Omit || !Null || !State || !Pretty
        || !Root->FindPropertyNameFromGuid(Score->Field).ToString().StartsWith(TEXT("DesignerScore_"), ESearchCase::CaseSensitive)
        || Root->FindPropertyNameFromGuid(Type->Field).ToString().Contains(TEXT("@"))
        || Omit->Presence != EVaRestJsonPresence::OmitWhenDefault
        || Null->Presence != EVaRestJsonPresence::NullWhenDefault || !Null->bNullable)
        return Fail(TEXT("Imported GUID rename, sanitized @type or presence rules differ"));
    // Physical defaults and the unset enum are observed through the real converter, not repaired by this fixture.
    FStructOnScope Defaults(Root);
    const FName StateName = Root->FindPropertyNameFromGuid(State->Field);
    FProperty* StateProperty = StateName.IsNone() ? nullptr : FindFProperty<FProperty>(Root, StateName);
    const auto* TypedState = CastField<FEnumProperty>(StateProperty);
    const auto* StateStorage = TypedState ? TypedState->GetUnderlyingProperty() : CastField<FNumericProperty>(StateProperty);
    UEnum* StateEnum = TypedState ? TypedState->GetEnum() : (StateStorage ? StateStorage->GetIntPropertyEnum() : nullptr);
    bool bPhysicalUnset = false;
    if (StateStorage && StateEnum)
    {
        const int64 Stored = StateStorage->GetSignedIntPropertyValue(StateProperty->ContainerPtrToValuePtr<void>(Defaults.GetStructMemory()));
        for (const auto& Rules : Api->JsonTypes)
            if (Rules.Type.Get() == StateEnum)
                for (const auto& Rule : Rules.Enums)
                    if (Rule.bUnset)
                    {
                        const int32 Index = StateEnum->GetIndexByName(Rule.Enumerator);
                        bPhysicalUnset |= Index != INDEX_NONE && StateEnum->GetValueByIndex(Index) == Stored;
                    }
    }
    if (!bPhysicalUnset) return Fail(TEXT("Imported optional state does not physically hold its actual mapped unset enumerator"));
    if (!Golden(FVaRestJsonConvert::StructToJson(Root, Defaults.GetStructMemory()), TEXT("wire-ONE")))
        return Fail(TEXT("First default conversion lost imported defaults/null/omission/unset semantics"));
    const auto Input = FVaRestJson::Parse(FUtf8StringView(UTF8TEXT(
        "{\"Wire.Score\":7,\"@type\":\"alpha\",\"omitCount\":42,\"nullCount\":42,\"pretty\":\"wire-TWO\"}")));
    FStructOnScope Instance(Root);
    FVaRestJsonDecodeReport Report;
    if (!Input.HasValue() || !FVaRestJsonConvert::JsonToStruct(Input.GetValue(), Root, Instance.GetStructMemory(),
            FVaRestJsonConvertOptions(), &Report) || Report.Errors.Num() || Report.Warnings.Num() || Report.bBudgetExceeded)
        return Fail(TEXT("Imported default-options decode produced a diagnostic"));
    Wire = FVaRestJsonConvert::StructToString(Root, Instance.GetStructMemory());
    const auto Actual = FVaRestJson::Parse(FStringView(Wire));
    if (!Actual.HasValue() || !Golden(Actual.GetValue(), TEXT("wire-TWO")))
        return Fail(TEXT("Imported renamed/prettified six-feature wire differs from the independent semantic golden"));
    Error.Reset();
    return true;
}
void Start(bool bCooked)
{
    check(IsInGameThread());
    if (Proof) return;
    Proof = MakeShared<FProof, ESPMode::ThreadSafe>(bCooked);
    Proof->Arm();
}
void Stop()
{
    check(IsInGameThread());
    if (Proof) { Proof->Shutdown(); Proof.Reset(); }
}
int32 GetOutcome(FString& Detail, FString& Wire)
{
    check(IsInGameThread());
    if (!Proof) { Detail = TEXT("O4 early witness was not armed"); Wire.Reset(); return -1; }
    Detail = Proof->Detail; Wire = Proof->Wire;
    return Proof->Outcome;
}
}
