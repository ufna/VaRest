// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestM5EndpointRunner.h"
#include "VaRestM5EndpointContract.h"
#include "CoreGlobals.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Templates/Function.h"
#include "UObject/UnrealType.h"
#include "VaRestApi.h"
#include "VaRestApiClient.h"
#include "VaRestHub.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"
DEFINE_LOG_CATEGORY_STATIC(LogVaRestM5Endpoint,Log,All);
namespace
{
bool WriteEndpointProof(bool bCreate, TFunctionRef<FVaRestJson()> MakeRow)
{
    FString ProofPath;
    if (!FParse::Value(FCommandLine::Get(),TEXT("VaRestM5EndpointProof="),ProofPath)) return true;
    if (ProofPath.IsEmpty() || FPaths::IsRelative(ProofPath)
        || !IFileManager::Get().DirectoryExists(*FPaths::GetPath(ProofPath))
        || (bCreate ? IFileManager::Get().FileExists(*ProofPath) : !IFileManager::Get().FileExists(*ProofPath))) return false;
    const auto Encoded=MakeRow().TryToString(EVaRestJsonFormat::Condensed);
    if (!Encoded.HasValue()) return false;
    return FFileHelper::SaveStringToFile(Encoded.GetValue()+TEXT("\n"),*ProofPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,&IFileManager::Get(),
        bCreate ? FILEWRITE_NoReplaceExisting : FILEWRITE_Append);
}
bool EndpointProofWriteFailure()
{
    UE_LOG(LogVaRestM5Endpoint,Error,TEXT("VAREST_M5_ENDPOINT_PROOF_WRITE_FAILED"));
    FPlatformMisc::RequestExitWithStatus(false,72,TEXT("VaRestM5EndpointProof"));
    return false;
}
}
AVaRestM5EndpointRunner::AVaRestM5EndpointRunner() { PrimaryActorTick.bCanEverTick=true; }
bool AVaRestM5EndpointRunner::Fail(const FString& Reason)
{
    const bool bWasArmed=bArmed;
    bArmed=false;
    if (bWasArmed && !WriteEndpointProof(false,[this,&Reason]
        { return FVaRestJson::Object().Set(TEXT("schema"),TEXT("VaRest.M5.EndpointProof.1"))
            .Set(TEXT("event"),TEXT("FAIL")).Set(TEXT("case"),NextCase)
            .Set(TEXT("reason"),Reason).Set(TEXT("gameThread"),IsInGameThread()); }))
        return EndpointProofWriteFailure();
    UE_LOG(LogVaRestM5Endpoint,Error,TEXT("VAREST_M5_ENDPOINT_FAIL case=%d reason=%s"),NextCase,*Reason);
    if (!GIsEditor && FParse::Param(FCommandLine::Get(),TEXT("VaRestM5EndpointsExit"))) FPlatformMisc::RequestExitWithStatus(false,72,TEXT("VaRestM5Endpoints"));
    return false;
}
bool AVaRestM5EndpointRunner::Arm()
{
    if (!IsInGameThread()) return Fail(TEXT("Fixture reflected continuation left GameThread"));
    if (!FParse::Param(FCommandLine::Get(),TEXT("VaRestM5Endpoints")) || IsRunningCommandlet()) return false;
    const auto RefuseArm=[this](const TCHAR* Branch)
    {
        const auto* Defaults=Cast<AVaRestM5EndpointRunner>(GetClass()->GetDefaultObject(false)); // observation only; never create/load a CDO
        const FString ActualApi=Api ? Api.GetPathName():TEXT("<null>");
        const FString DefaultApi=Defaults && Defaults->Api ? Defaults->Api.GetPathName():TEXT("<null>");
        return Fail(FString::Printf(TEXT("Arm refused branch=%s bArmed=%d bPassed=%d actor=%s class=%s actorIsCDO=%d api=%s expectedApi=%s cdoPresent=%d cdoApi=%s"),
            Branch,bArmed ? 1:0,bPassed ? 1:0,*GetPathName(),*GetClass()->GetPathName(),HasAnyFlags(RF_ClassDefaultObject) ? 1:0,
            *ActualApi,VaRestM5Endpoint::ApiPath,Defaults ? 1:0,*DefaultApi));
    };
    if (bArmed) return RefuseArm(TEXT("AlreadyArmed"));
    if (bPassed) return RefuseArm(TEXT("AlreadyPassed"));
    if (!Api) return RefuseArm(TEXT("ApiNull"));
    if (!Api->GetPathName().Equals(VaRestM5Endpoint::ApiPath,ESearchCase::CaseSensitive)) return RefuseArm(TEXT("ApiPathMismatch"));
    UVaRestHub* Hub=UVaRestHub::Get(this); Client=Hub ? Hub->GetClient(Api) : nullptr;
    if (!Client || !Client->GetBaseUrl().StartsWith(TEXT("http://127.0.0.1:"),ESearchCase::CaseSensitive)) return Fail(TEXT("No ordinary asset client/owned loopback base URL"));
    const auto* Payload=FindFProperty<FStructProperty>(GetClass(),TEXT("Payload"));
    if (!Payload || !Payload->Struct->GetPathName().Equals(VaRestM5Endpoint::RootPath,ESearchCase::CaseSensitive)) return Fail(TEXT("Serialized A payload type is unavailable"));
    Payload->Struct->PrepareCppStructOps(); // Real public lifecycle preparation; no native registration or worker opt-in.
    if (Payload->Struct->GetCppStructOps()!=nullptr
        || !FVaRestJsonConvert::StructToString(Payload->Struct,Payload->ContainerPtrToValuePtr<void>(this)).Equals(VaRestM5Endpoint::WireBody(),ESearchCase::CaseSensitive))
        return Fail(TEXT("Serialized Blueprint payload lost exact default-conversion semantics"));
    bArmed=true; Deadline=FPlatformTime::Seconds()+30;
    if (!WriteEndpointProof(true,[this]
        { return FVaRestJson::Object().Set(TEXT("schema"),TEXT("VaRest.M5.EndpointProof.1"))
            .Set(TEXT("event"),TEXT("ARM")).Set(TEXT("cases"),8).Set(TEXT("nextCase"),NextCase)
            .Set(TEXT("actor"),GetPathName()).Set(TEXT("class"),GetClass()->GetPathName())
            .Set(TEXT("api"),Api.GetPathName()).Set(TEXT("gameThread"),IsInGameThread()); }))
    { bArmed=false; return EndpointProofWriteFailure(); }
    UE_LOG(LogVaRestM5Endpoint,Display,TEXT("VAREST_M5_ENDPOINT_ARMED mode=GameThread reflectedWorkers=disabled UDSNativeOps=0 requests=realBlueprintNodes cases=8"));
    return true;
}
bool AVaRestM5EndpointRunner::CheckCase(int32 Case,const FVaRestResponseData& Http,const FVaRestError& Error,bool bErrorBodyValid)
{
    using namespace VaRestM5Endpoint;
    if (!IsInGameThread()) return Fail(TEXT("Fixture reflected continuation left GameThread"));
    if (!bArmed || Case!=NextCase || Case<0 || Case>7) return Fail(TEXT("Missing/duplicate/out-of-order Blueprint continuation"));
    uint64 RequestId=0;
    if (!VaRest::Http::TryParseUnsignedId(Http.RequestId,RequestId) || !RequestId || RequestIds.Contains(Http.RequestId)
        || Http.Status!=(Case==6 ? 422:200) || !Http.ContentType.Equals(TEXT("application/json"),ESearchCase::CaseSensitive) || Http.Attempts!=1 || Http.bFromCache || Http.DecodeWarnings.Num()) return Fail(TEXT("Actual HTTP identity/status/attempt/decode metadata differs"));
    if ((Case==6 && (Error.Kind!=EVaRestErrorKind::Http || Error.HttpStatus!=422 || !bErrorBodyValid))
        || (Case!=6 && Error.IsError())) return Fail(TEXT("Actual typed HTTP error/success outcome differs"));
    TArray<FString> Replies;
    for (const auto& Header:Http.Headers) if (Header.Name.Equals(TEXT("X-M5-Reply"),ESearchCase::IgnoreCase)) Replies.Add(Header.Value);
    if (Replies.Num()!=2 || !Replies[0].Equals(TEXT("alpha"),ESearchCase::CaseSensitive) || !Replies[1].Equals(TEXT("omega"),ESearchCase::CaseSensitive)) return Fail(TEXT("Ordered repeated response headers differ"));
    const FTCHARToUTF8 ExpectedBytes(*ReplyBody(Case));
    if (Http.Body.Num()!=ExpectedBytes.Length() || FMemory::Memcmp(Http.Body.GetData(),ExpectedBytes.Get(),Http.Body.Num())!=0) return Fail(TEXT("Actual raw HTTP response bytes differ"));
    FProperty* Output=FindFProperty<FProperty>(GetClass(),FName(*FString::Printf(TEXT("Output%d"),Case)));
    if (!Output) return Fail(TEXT("Actual typed Blueprint output storage is absent"));
    auto Expected=FVaRestJson::Parse(FStringView(Case==0 || Case==1 || Case==6 ? WireBody():ReplyBody(Case)));
    const auto Actual=FVaRestJsonConvert::PropertyToJson(Output,Output->ContainerPtrToValuePtr<void>(this));
    if (!Expected.HasValue() || !Actual.Equals(Expected.GetValue())) return Fail(TEXT("Real typed Blueprint output/default converter semantics differ"));
    if (Case==0 || Case==1 || Case==6)
    {
        const auto* Struct=CastField<FStructProperty>(Output);
        if (!Struct || !Struct->Struct->GetPathName().Equals(RootPath,ESearchCase::CaseSensitive)) return Fail(TEXT("A typed root identity differs"));
        const auto* DecodeOnly=CastField<FStrProperty>(RuleProperty(Api,Struct->Struct,TEXT("decodeOnly")));
        const auto* Ignored=CastField<FStrProperty>(RuleProperty(Api,Struct->Struct,TEXT("ignored")));
        const void* Memory=Output->ContainerPtrToValuePtr<void>(this);
        if (!DecodeOnly || !Ignored || !DecodeOnly->GetPropertyValue_InContainer(Memory).Equals(TEXT("server"),ESearchCase::CaseSensitive)
            || !Ignored->GetPropertyValue_InContainer(Memory).Equals(TEXT("kept"),ESearchCase::CaseSensitive)) return Fail(TEXT("Real DecodeOnly/Ignore typed storage differs"));
    }
    if (!WriteEndpointProof(false,[Case,&Http,&Replies,&Actual]
        {
            auto Headers=FVaRestJson::Array();
            for (const auto& Reply:Replies) Headers.Add(Reply);
            return FVaRestJson::Object().Set(TEXT("schema"),TEXT("VaRest.M5.EndpointProof.1"))
                .Set(TEXT("event"),TEXT("CASE")).Set(TEXT("case"),Case).Set(TEXT("status"),Http.Status)
                .Set(TEXT("requestId"),Http.RequestId).Set(TEXT("typedOutput"),true).Set(TEXT("typedJson"),Actual)
                .Set(TEXT("rawBytes"),true).Set(TEXT("rawBodyBytes"),Http.Body.Num())
                .Set(TEXT("repeatedHeaders"),true).Set(TEXT("replyHeaders"),Headers).Set(TEXT("gameThread"),IsInGameThread());
        }))
    { bArmed=false; return EndpointProofWriteFailure(); }
    RequestIds.Add(Http.RequestId); ++NextCase;
    UE_LOG(LogVaRestM5Endpoint,Display,TEXT("VAREST_M5_ENDPOINT_CASE case=%d status=%d typedOutput=1 rawBytes=1 repeatedHeaders=1 mode=GameThread"),Case,Http.Status);
    if (NextCase==8)
    {
        bArmed=false; bPassed=true;
        if (!WriteEndpointProof(false,[this]
            { return FVaRestJson::Object().Set(TEXT("schema"),TEXT("VaRest.M5.EndpointProof.1"))
                .Set(TEXT("event"),TEXT("PASS")).Set(TEXT("cases"),NextCase)
                .Set(TEXT("uniqueRequestIds"),RequestIds.Num()).Set(TEXT("gameThread"),IsInGameThread()); }))
        { bPassed=false; return EndpointProofWriteFailure(); }
        UE_LOG(LogVaRestM5Endpoint,Display,TEXT("VAREST_M5_ENDPOINT_PASS proof=A cases=8 realBlueprintNodes=1 mode=GameThread workerClosure=UNPROVED wholeTask14=OPEN"));
        if (!GIsEditor && FParse::Param(FCommandLine::Get(),TEXT("VaRestM5EndpointsExit"))) FPlatformMisc::RequestExitWithStatus(false,0,TEXT("VaRestM5Endpoints"));
    }
    return true;
}
void AVaRestM5EndpointRunner::Reject(const FString& Reason,const FVaRestResponseData& Http,const FVaRestError& Error)
{
    Fail(FString::Printf(TEXT("%s status=%d kind=%d"),*Reason,Http.Status,static_cast<int32>(Error.Kind)));
}
void AVaRestM5EndpointRunner::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (bArmed && FPlatformTime::Seconds()>Deadline) Fail(TEXT("Owned finite Blueprint fixture deadline exceeded"));
}
