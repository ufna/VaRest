// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestM5EndpointContract.h"
#include "VaRestM5EndpointRunner.h"
#include "CoreGlobals.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/PlatformTime.h"
#include "K2Node.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "VaRestApi.h"
namespace VaRestM5EndpointPIE
{
constexpr TCHAR BPPath[]=TEXT("/VaRestM5Smoke/Endpoint001/SM5_Runner.SM5_Runner");
constexpr TCHAR MapPath[]=TEXT("/VaRestM5Smoke/Endpoint001/SM5_Map");
bool Dedicated()
{
    return GIsEditor && !IsRunningCommandlet() && FParse::Param(FCommandLine::Get(),TEXT("VaRestM5EndpointPIE"))
        && FParse::Param(FCommandLine::Get(),TEXT("VaRestM5Endpoints")) && FParse::Param(FCommandLine::Get(),TEXT("unattended"))
        && FParse::Param(FCommandLine::Get(),TEXT("nowrite")) && !FParse::Param(FCommandLine::Get(),TEXT("VaRestM5EndpointsExit"));
}
bool Matches(const FVaRestTypeRef& T,int32 Case,const UObject* Root,const UObject* Enum)
{
    const EVaRestTypeKind Kinds[]={EVaRestTypeKind::Struct,EVaRestTypeKind::Struct,EVaRestTypeKind::Int,EVaRestTypeKind::Int,EVaRestTypeKind::Enum,EVaRestTypeKind::DateTime,EVaRestTypeKind::Struct,EVaRestTypeKind::Int};
    const auto Container=Case==2 ? EVaRestTypeContainer::Array:Case==3 ? EVaRestTypeContainer::Map:EVaRestTypeContainer::None;
    const UObject* Object=Case==0 || Case==1 || Case==6 ? Root:Case==4 ? Enum:nullptr;
    return T.Kind==Kinds[Case] && T.Container==Container && T.Type.Get()==Object && !T.bJsonValue;
}
struct FRun : IAutomationLatentCommand
{
    FAutomationTestBase& Test;
    TStrongObjectPtr<UBlueprint> BP;
    TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
    TWeakObjectPtr<UClass> SavedClass;
    TArray<uint8> SavedScript;
    const FProperty* SavedPropertyLink=nullptr;
    int32 SavedPropertiesSize=0,PreparedCompileEvents=0,OwnedPreCompiles=0,PreparedOwnedPreCompiles=0;
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<AVaRestM5EndpointRunner> Runner;
    FDelegateHandle PreCompile,Compiled,Begin,End;
    int32 Compiles=0,Begins=0,Ends=0,Stage=0;
    bool Failed=false,Observed=false,Requested=false;
    double Deadline=0;
    explicit FRun(FAutomationTestBase& InTest):Test(InTest) {}
    ~FRun()
    {
        EndOwnedSession();
        if (GEditor) { GEditor->OnBlueprintPreCompile().Remove(PreCompile);GEditor->OnBlueprintCompiled().Remove(Compiled); }
        FEditorDelegates::BeginPIE.Remove(Begin);FEditorDelegates::EndPIE.Remove(End);
    }
    void EndOwnedSession()
    {
        if (!Requested || !GEditor) return;
        // Preserve active SDK session data until ordinary EndPlayMap has restored/closed the session.
        if (GEditor->PlayWorld || GEditor->GetPlayInEditorSessionInfo().IsSet()) GEditor->RequestEndPlayMap();
        else if (GEditor->GetPlaySessionRequest().IsSet()) GEditor->CancelRequestPlaySession(); // queued-only; no active world/info
    }
    bool Error(const TCHAR* Message) { if (!Failed) Test.AddError(Message);Failed=true;return false; }
    int32 PIEWorlds() const { int32 Count=0;for (const auto& C:GEngine->GetWorldContexts()) if (C.WorldType==EWorldType::PIE) ++Count;return Count; }
    bool Unchanged() const
    {
        const auto* Class=BP ? Cast<UBlueprintGeneratedClass>(BP->GeneratedClass):nullptr;
        return BP && OwnedPreCompiles==PreparedOwnedPreCompiles && BP->IsUpToDate() && Class && Class==SavedClass.Get()
            && Class->GetPropertiesSize()==SavedPropertiesSize && Class->PropertyLink==SavedPropertyLink
            && Class->UberGraphFunction && Class->UberGraphFunction->Script==SavedScript;
    }
    bool Inspect()
    {
        using namespace VaRestM5Endpoint;
        if (!Dedicated() || !GEditor || !GEngine || GEditor->PlayWorld || GEditor->GetPlaySessionRequest().IsSet() || GEditor->GetPlayInEditorSessionInfo().IsSet() || PIEWorlds())
            return Error(TEXT("Dedicated ordinary saved-BP/map process with no play session required"));
        PreCompile=GEditor->OnBlueprintPreCompile().AddLambda([this](UBlueprint* InBP) { ++Compiles;if (InBP && InBP->GetPathName()==BPPath) ++OwnedPreCompiles; });
        Compiled=GEditor->OnBlueprintCompiled().AddLambda([this]() { ++Compiles; });
        BP.Reset(LoadObject<UBlueprint>(nullptr,BPPath));
        if (!BP || !FEditorFileUtils::LoadMap(MapPath,false,false)) return Error(TEXT("Ordinary saved Blueprint/map load failed"));
        // Preparation may compile/reinstance normally. Obtain no class/CDO/property pointer until it finishes.
        FCompilerResultsLog PreparationLog;
        FKismetEditorUtilities::CompileBlueprint(BP.Get(),EBlueprintCompileOptions::SkipGarbageCollection|EBlueprintCompileOptions::SkipSave,&PreparationLog);
        Test.AddInfo(FString::Printf(TEXT("VAREST_M5_PIE_PREPARATION compileErrors=%d compileWarnings=%d compileEvents=%d ownedPreCompileEvents=%d saves=disabled"),PreparationLog.NumErrors,PreparationLog.NumWarnings,Compiles,OwnedPreCompiles));
        if (PreparationLog.NumErrors) return Error(TEXT("Ordinary saved Blueprint preparation compile failed"));
        auto* Class=BP ? Cast<UBlueprintGeneratedClass>(BP->GeneratedClass):nullptr;
        auto* CDO=Class ? Cast<AVaRestM5EndpointRunner>(Class->GetDefaultObject()):nullptr;
        // Observe resolved state only; unresolved parent/Uber observations stay -1 rather than loading.
        const bool ParentResolved=BP && BP->ParentClass.GetGCPtr().IsResolved();
        const bool UberResolved=Class && Class->UberGraphFunction.IsResolved();
        const auto* ObservedUber=Class && UberResolved ? Class->UberGraphFunction.Get():nullptr;
        const FString ObservedParentPath=BP ? BP->ParentClass.GetGCPtr().GetPathName():TEXT("<no-BP>");
        const FString ObservedApiPath=CDO && CDO->Api ? CDO->Api.GetPathName():CDO ? TEXT("<null>"):TEXT("<no-CDO>");
        Test.AddInfo(FString::Printf(TEXT("VAREST_M5_PIE_PREPARED_GUARD_OBSERVED bp=%d class=%d uber=%d uberResolved=%d scriptBytes=%d upToDate=%d status=%d regenerated=%d compiles=%d parentResolved=%d parentMatches=%d parentPath=%s cdo=%d bPassed=%d clientPresent=%d apiPresent=%d apiPath=%s"),
            BP ? 1:0,Class ? 1:0,Class ? (Class->UberGraphFunction ? 1:0):INDEX_NONE,UberResolved ? 1:0,ObservedUber ? ObservedUber->Script.Num():INDEX_NONE,
            BP ? (BP->IsUpToDate() ? 1:0):INDEX_NONE,BP ? static_cast<int32>(BP->Status):INDEX_NONE,BP ? (BP->bHasBeenRegenerated ? 1:0):INDEX_NONE,Compiles,
            ParentResolved ? 1:0,BP && Class && ParentResolved ? (BP->ParentClass.Get()==AVaRestM5EndpointRunner::StaticClass() ? 1:0):INDEX_NONE,*ObservedParentPath,
            CDO ? 1:0,CDO ? (CDO->bPassed ? 1:0):INDEX_NONE,CDO ? (CDO->Client ? 1:0):INDEX_NONE,CDO ? (CDO->Api ? 1:0):INDEX_NONE,*ObservedApiPath));
        if (!Class || !Class->UberGraphFunction || Class->UberGraphFunction->Script.IsEmpty() || !BP->IsUpToDate()
            || BP->ParentClass.Get()!=AVaRestM5EndpointRunner::StaticClass() || !CDO || CDO->bPassed || CDO->Client || !CDO->Api || !CDO->Api->GetPathName().Equals(ApiPath,ESearchCase::CaseSensitive)) return Error(TEXT("Prepared generated class/bytecode/default API unavailable"));
        const int32 InspectionOwnedPreCompiles=OwnedPreCompiles,InspectionSize=Class->GetPropertiesSize();
        const FProperty* InspectionPropertyLink=Class->PropertyLink;TArray<uint8> InspectionScript=Class->UberGraphFunction->Script;
        const auto* Environment=CDO->Api->FindEnvironment(CDO->Api->DefaultEnvironment);
        if (!Environment || Environment->BaseUrl!=TEXT("http://127.0.0.1:18765")) return Error(TEXT("Saved owned server port differs"));
        UObject *Root=nullptr,*Enum=nullptr;for (const auto& T:CDO->Api->JsonTypes) if (T.Type) { if (T.Type->GetPathName()==RootPath) Root=T.Type.Get();if (T.Type->GetPathName()==EnumPath) Enum=T.Type.Get(); }
        const auto* Payload=FindFProperty<FStructProperty>(Class,TEXT("Payload"));
        if (!Root || !Enum || !Payload || Payload->Struct!=Root) return Error(TEXT("Actual saved A payload/type descriptors missing"));
        TArray<UK2Node*> Nodes;FBlueprintEditorUtils::GetAllNodesOfClass(BP.Get(),Nodes);TSet<int32> Cases;int32 Assignments=0;
        for (auto* N:Nodes)
        {
            if (!N) return Error(TEXT("Missing saved graph node"));const FString Name=N->GetClass()->GetPathName();const bool Endpoint=Name==TEXT("/Script/VaRestEditor.K2Node_VaRestCallEndpoint");
            if (!Endpoint && Name!=TEXT("/Script/VaRestEditor.K2Node_VaRestSendRequest")) continue;
            int32 Case=INDEX_NONE;const FVaRestTypeRef* Type=nullptr;
            if (Endpoint)
            {
                const auto* A=FindFProperty<FObjectPropertyBase>(N->GetClass(),TEXT("Api"));const auto* ID=FindFProperty<FStructProperty>(N->GetClass(),TEXT("EndpointId"));
                const auto* E=ID && ID->Struct==TBaseStructure<FGuid>::Get() ? CDO->Api->FindEndpoint(*ID->ContainerPtrToValuePtr<FGuid>(N)):nullptr;
                if (!A || A->GetObjectPropertyValue_InContainer(N)!=CDO->Api || !E) return Error(TEXT("Saved genuine endpoint API/GUID config differs"));
                for (int32 I:{0,2,4,6}) if (E->Name==FName(*FString::Printf(TEXT("Case%d"),I))) Case=I;
                if (Case==INDEX_NONE || E->Verb!=TEXT("POST") || E->Path!=FString::Printf(TEXT("/m5/endpoint/%d"),Case)) return Error(TEXT("Saved endpoint route differs"));
                if (!Matches(E->RequestType,0,Root,Enum) || E->BodyKind!=EVaRestEndpointBodyKind::Json || E->AuthMode!=EVaRestAuthMode::None) return Error(TEXT("Saved endpoint body/auth config differs"));
                Type=&E->ResponseType;if (Case==6 && !Matches(E->ErrorType,0,Root,Enum)) return Error(TEXT("Saved typed error descriptor differs"));
            }
            else
            {
                const auto* Path=N->FindPin(TEXT("Path"));const auto* Verb=N->FindPin(TEXT("Verb"));const auto* T=FindFProperty<FStructProperty>(N->GetClass(),TEXT("ResponseType"));
                for (int32 I:{1,3,5,7}) if (Path && Path->DefaultValue==FString::Printf(TEXT("/m5/send/%d"),I)) Case=I;
                if (Case==INDEX_NONE || !Verb || Verb->DefaultValue!=TEXT("POST") || !T || T->Struct!=FVaRestTypeRef::StaticStruct()) return Error(TEXT("Saved genuine Send Request route/config differs"));
                const auto* Auth=N->FindPin(TEXT("AuthMode"));const auto* Client=N->FindPin(TEXT("Client"));
                if (!Auth || Auth->DefaultValue!=StaticEnum<EVaRestAuthMode>()->GetNameStringByValue(static_cast<int64>(EVaRestAuthMode::None)) || !Client || Client->LinkedTo.Num()!=1) return Error(TEXT("Saved Send Request auth/client config differs"));
                Type=T->ContainerPtrToValuePtr<FVaRestTypeRef>(N);
            }
            if (!N->GetClass()->HasAnyClassFlags(CLASS_Native) || Cases.Contains(Case) || !Type || !Matches(*Type,Case,Root,Enum)) return Error(TEXT("Eight genuine native K2 types/configs differ"));
            Test.AddInfo(FString::Printf(TEXT("VAREST_M5_PIE_SAVED_NODE case=%d endpoint=%d kind=%d container=%d"),Case,Endpoint ? 1:0,static_cast<int32>(Type->Kind),static_cast<int32>(Type->Container)));
            FEdGraphPinType BodyType;const auto* Body=N->FindPin(TEXT("Body"));
            if (!Body || Body->LinkedTo.Num()!=1 || !GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Payload,BodyType) || Body->PinType!=BodyType) return Error(TEXT("Saved typed root body link/shape differs"));
            Cases.Add(Case);const FName Variable(*FString::Printf(TEXT("Output%d"),Case));auto* Output=N->FindPin(Case==6 ? TEXT("ErrorBody"):TEXT("Response"));
            FEdGraphPinType Expected;const auto* Property=FindFProperty<FProperty>(Class,Variable);
            if (!Output || !Property || !GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property,Expected) || Output->PinType!=Expected) return Error(TEXT("Saved actual typed output/member shape differs"));
            int32 Linked=0;for (const auto* P:Output->LinkedTo) if (P) if (const auto* Set=Cast<UK2Node_VariableSet>(P->GetOwningNodeUnchecked());Set && Set->VariableReference.GetMemberName()==Variable && P==Set->FindPin(Variable)) ++Linked;
            if (Linked!=1) return Error(TEXT("Actual typed output assignment is absent/ambiguous"));++Assignments;
        }
        if (Cases.Num()!=8 || Assignments!=8) return Error(TEXT("Prepared eight-node bytecode/assignments not intact"));
        UWorld* EditorWorld=GEditor->GetEditorWorldContext().World();int32 Placed=0;
        if (!EditorWorld || EditorWorld->GetOutermost()->GetName()!=MapPath) return Error(TEXT("Wrong loaded saved map"));
        for (TActorIterator<AVaRestM5EndpointRunner> It(EditorWorld);It;++It) { if (It->GetClass()!=Class || It->bPassed || It->Client) return Error(TEXT("Saved placed runner class/state differs"));++Placed; }
        if (Placed!=1) return Error(TEXT("Exactly one ordinary placed runner required"));
        Settings.Reset(DuplicateObject<ULevelEditorPlaySettings>(GetDefault<ULevelEditorPlaySettings>(),GetTransientPackage()));
        Settings->AutoRecompileBlueprints=false;Settings->SetRunUnderOneProcess(true);Settings->SetPlayNetMode(PIE_Standalone);Settings->SetPlayNumberOfClients(1);
        Begin=FEditorDelegates::BeginPIE.AddLambda([this](bool Simulate) { ++Begins;if (Simulate) Error(TEXT("SIE is forbidden")); });
        End=FEditorDelegates::EndPIE.AddLambda([this](bool Simulate) { ++Ends;if (Simulate) Error(TEXT("SIE teardown is forbidden")); });
        if (OwnedPreCompiles!=InspectionOwnedPreCompiles || BP->GeneratedClass!=Class || !Class->UberGraphFunction || Class->UberGraphFunction->Script!=InspectionScript
            || Class->GetPropertiesSize()!=InspectionSize || Class->PropertyLink!=InspectionPropertyLink) return Error(TEXT("Owned Blueprint changed during prepared inspection"));
        SavedClass=Class;SavedScript=MoveTemp(InspectionScript);SavedPropertiesSize=InspectionSize;SavedPropertyLink=InspectionPropertyLink;
        PreparedCompileEvents=Compiles;PreparedOwnedPreCompiles=OwnedPreCompiles;
        Test.AddInfo(FString::Printf(TEXT("VAREST_M5_PIE_PREPARED_INSPECT profile=OrdinarySaved nodes=8 assignments=8 preparedBytecode=1 scriptBytes=%d propertiesSize=%d preparationCompileEvents=%d preparationOwnedPreCompileEvents=%d"),SavedScript.Num(),SavedPropertiesSize,PreparedCompileEvents,PreparedOwnedPreCompiles));return true;
    }
    void Stop() { EndOwnedSession();Stage=2;Deadline=FPlatformTime::Seconds()+15; }
    virtual bool Update() override
    {
        if (!GEditor || !GEngine) { Error(TEXT("Editor vanished"));return true; }
        if (Stage==0)
        {
            FRequestPlaySessionParams Params;Params.SessionDestination=EPlaySessionDestinationType::InProcess;Params.WorldType=EPlaySessionWorldType::PlayInEditor;Params.EditorPlaySettings=Settings.Get();Params.bAllowOnlineSubsystem=false;
            if (!FAutomationEditorCommonUtils::SetPlaySessionStartToActiveViewport(Params)) { Error(TEXT("Real Slate level viewport required; use Root Xvfb/default-RHI Editor profile"));return true; }
            Requested=true;GEditor->RequestPlaySession(Params);Stage=1;Deadline=FPlatformTime::Seconds()+45;return false;
        }
        if (!Unchanged()) { Error(TEXT("Prepared Blueprint compiled or bytecode/class/layout changed during PIE"));if (Stage!=2) Stop(); }
        if (Stage==1)
        {
            UWorld* PIE=GEditor->PlayWorld;
            if (PIE)
            {
                if (PIE->WorldType!=EWorldType::PIE || PIEWorlds()!=1 || GEditor->IsSimulateInEditorInProgress()) { Error(TEXT("Requires one real in-process PIE world"));Stop();return false; }
                World=PIE;int32 Count=0;AVaRestM5EndpointRunner* Actual=nullptr;for (TActorIterator<AVaRestM5EndpointRunner> It(PIE);It;++It) { Actual=*It;++Count; }
                if (PIE->HasBegunPlay() && (Count!=1 || !Actual || Actual->GetClass()!=SavedClass.Get())) { Error(TEXT("Exactly one placed saved-class PIE runner required"));Stop();return false; }
                if (Actual) { Runner=Actual;if (Actual->bPassed && PIE->HasBegunPlay()) { Observed=true;Test.AddInfo(TEXT("VAREST_M5_PIE_RUNNER_OBSERVED bPassed=1 world=PIE source=normalBeginPlay"));Stop();return false; } }
            }
            if (Failed || Ends || FPlatformTime::Seconds()>Deadline) { Error(TEXT("PIE failed/ended or runner deadline30+grace15 expired"));Stop(); }return false;
        }
        if (!GEditor->PlayWorld && !PIEWorlds() && !World.IsValid() && !Runner.IsValid() && !GEditor->GetPlaySessionRequest().IsSet() && !GEditor->GetPlayInEditorSessionInfo().IsSet())
        {
            Test.AddInfo(FString::Printf(TEXT("VAREST_M5_PIE_CLEANUP_OBSERVED profile=OrdinarySaved BeginPIE=%d EndPIE=%d playWorldGone=1 worldGone=1 runnerGone=1 playRequestGone=1 activeInfoGone=1 observedRunner=%d failed=%d"),Begins,Ends,Observed ? 1:0,Failed ? 1:0));
            Requested=false;if (!Observed || Begins!=1 || Ends!=1 || !Unchanged()) Error(TEXT("Missing actual runner completion/BeginPIE/EndPIE or unchanged prepared class"));
            if (!Failed) Test.AddInfo(FString::Printf(TEXT("VAREST_M5_PIE_PASS profile=OrdinarySaved actualPIE=1 runnerCompleted=1 EndPIE=1 worldGone=1 activeInfoGone=1 preparationCompileEvents=%d preparationOwnedPreCompileEvents=%d measuredCompileEvents=%d measuredOwnedPreCompileEvents=%d classScriptLayoutUnchanged=1 wireJournal=RootRequired"),PreparedCompileEvents,PreparedOwnedPreCompiles,Compiles-PreparedCompileEvents,OwnedPreCompiles-PreparedOwnedPreCompiles));return true;
        }
        if (FPlatformTime::Seconds()>Deadline) { Error(TEXT("Real EndPIE/world destruction grace15 expired"));return true; }return false;
    }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestM5EndpointPIE,"VaRest.M5.Endpoint.PIE",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
void FVaRestM5EndpointPIE::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const { if (VaRestM5EndpointPIE::Dedicated()) { Names.Add(TEXT("OrdinarySaved"));Commands.Add(TEXT("OrdinarySaved")); } }
bool FVaRestM5EndpointPIE::RunTest(const FString& Parameters)
{
    if (!VaRestM5EndpointPIE::Dedicated() || Parameters!=TEXT("OrdinarySaved")) { AddError(TEXT("Dedicated real Editor PIE flags required; EndpointsExit forbidden"));return false; }
    auto Run=MakeShared<VaRestM5EndpointPIE::FRun>(*this);if (!Run->Inspect()) return false;
    FAutomationTestFramework::Get().EnqueueLatentCommand(Run);return true;
}
#endif
