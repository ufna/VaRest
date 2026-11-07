// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
// Opt-in asset authoring only; the saved runtime example is a normal Blueprint.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "K2Node_VaRestCallEndpoint.h"
#include "VaRestApi.h"
#include "VaRestHttpTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace VaRestFirstLookAuthor
{
bool Dedicated()
{
    return FParse::Param(FCommandLine::Get(), TEXT("VaRestFirstLookFixture"))
        && FParse::Param(FCommandLine::Get(), TEXT("unattended"))
        && FParse::Param(FCommandLine::Get(), TEXT("nowrite"));
}
template<class T> T* Node(UEdGraph* Graph, int32 X, int32 Y)
{
    T* Result=NewObject<T>(Graph, NAME_None, RF_Transactional);
    Graph->AddNode(Result, false, false); Result->CreateNewGuid();
    Result->NodePosX=X; Result->NodePosY=Y; return Result;
}
UK2Node_CallFunction* Function(UEdGraph* Graph,UFunction* Fn,int32 X,int32 Y)
{
    if (!Fn) return nullptr;
    auto* Result=Node<UK2Node_CallFunction>(Graph,X,Y);
    Result->SetFromFunction(Fn); Result->AllocateDefaultPins(); return Result;
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestFirstLookAuthorTest,"VaRest.Examples.FirstLook.Author",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
void FVaRestFirstLookAuthorTest::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
    // No passing skip: this asset-authoring case is absent from ordinary discovery.
    if (VaRestFirstLookAuthor::Dedicated()) { Names.Add(TEXT("Northwind")); Commands.Add(TEXT("Northwind")); }
}
bool FVaRestFirstLookAuthorTest::RunTest(const FString& Parameters)
{
    using namespace VaRestFirstLookAuthor;
    if (!Dedicated() || Parameters!=TEXT("Northwind")) { AddError(TEXT("Requires sole dedicated nowrite authoring process")); return false; }
    const FString BlueprintPath=TEXT("/Game/VaRestFirstLook002/BP_NorthwindFirstLook");
    const FString MapPath=TEXT("/Game/VaRestFirstLook002/FirstLook");
    const FString OutputDir=FPaths::Combine(FPaths::ProjectContentDir(),TEXT("VaRestFirstLook002"));
    if (IFileManager::Get().DirectoryExists(*OutputDir) || FPackageName::DoesPackageExist(BlueprintPath) || FPackageName::DoesPackageExist(MapPath)
        || FindPackage(nullptr,*BlueprintPath) || FindPackage(nullptr,*MapPath))
    { AddError(TEXT("Owned demo namespace is occupied; no overwrite")); return false; }
    TStrongObjectPtr<UVaRestApi> Api(LoadObject<UVaRestApi>(nullptr,TEXT("/Game/FirstLook/DA_NorthwindGameApi.DA_NorthwindGameApi")));
    if (!TestNotNull(TEXT("actual saved Northwind input"),Api.Get())) return false;
    const FVaRestApiEndpoint* Selected=nullptr;
    for (const auto& E:Api->Endpoints)
    {
        if (E.Name==TEXT("GetPlayerProfile"))
        {
            if (Selected) { AddError(TEXT("Ambiguous saved endpoint")); return false; }
            Selected=&E;
        }
    }
    if (!TestNotNull(TEXT("actual selected endpoint"),Selected) || !TestTrue(TEXT("actual GUID and public wire path"),
        Selected->Id.IsValid() && Selected->Verb==TEXT("GET") && Selected->Path==TEXT("/profile"))) return false;
    TStrongObjectPtr<UPackage> Package(CreatePackage(*BlueprintPath));
    TStrongObjectPtr<UBlueprint> BP(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),Package.Get(),
        TEXT("BP_NorthwindFirstLook"),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass()));
    if (!TestNotNull(TEXT("normal Actor Blueprint"),BP.Get())) return false;
    BP->SetFlags(RF_Public|RF_Standalone|RF_Transactional);
    UEdGraph* Graph=FBlueprintEditorUtils::FindEventGraph(BP.Get());
    if (!TestNotNull(TEXT("actual event graph"),Graph)) return false;
    TArray<UK2Node_Event*> Events; FBlueprintEditorUtils::GetAllNodesOfClass(BP.Get(),Events);
    UK2Node_Event* Begin=nullptr;
    for (auto* Event:Events) if (Event->EventReference.GetMemberName()==TEXT("ReceiveBeginPlay"))
    { if (Begin) { AddError(TEXT("Duplicate BeginPlay")); return false; } Begin=Event; }
    if (!Begin)
    {
        Begin=Node<UK2Node_Event>(Graph,-650,0);
        Begin->EventReference.SetExternalMember(TEXT("ReceiveBeginPlay"),AActor::StaticClass());
        Begin->bOverrideFunction=true; Begin->AllocateDefaultPins();
    }
    Begin->NodePosX=-650; Begin->NodePosY=0;
    auto* Call=Node<UK2Node_VaRestCallEndpoint>(Graph,-300,0);
    Call->Api=Api.Get(); Call->EndpointId=Selected->Id; Call->EndpointName=Selected->Name; Call->AllocateDefaultPins();
    auto* Response=Call->FindPin(TEXT("Response"));
    UScriptStruct* Profile=Response ? Cast<UScriptStruct>(Response->PinType.PinSubCategoryObject.Get()) : nullptr;
    if (!TestNotNull(TEXT("genuine generated typed Profile output"),Profile)) return false;
    FProperty* DisplayName=nullptr;
    for (TFieldIterator<FProperty> It(Profile);It;++It)
        if (CastField<FStrProperty>(*It) && It->GetAuthoredName().Equals(TEXT("displayName"),ESearchCase::CaseSensitive))
        { if (DisplayName) { AddError(TEXT("Ambiguous authored displayName")); return false; } DisplayName=*It; }
    if (!TestNotNull(TEXT("real authored string property"),DisplayName)) return false;
    auto* Break=Node<UK2Node_BreakStruct>(Graph,180,200); Break->StructType=Profile; Break->AllocateDefaultPins();
    UEdGraphPin* StructInput=nullptr;
    for (auto* Pin:Break->Pins) if (Pin->Direction==EGPD_Input && Pin->PinType.PinCategory==UEdGraphSchema_K2::PC_Struct)
    { if (StructInput) { AddError(TEXT("Ambiguous break input")); return false; } StructInput=Pin; }
    auto* Concat=Function(Graph,UKismetStringLibrary::StaticClass()->FindFunctionByName(
        GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary,Concat_StrStr)),550,200);
    UFunction* Print=UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary,PrintString));
    auto* Success=Function(Graph,Print,950,0);auto* Error=Function(Graph,Print,950,450);auto* Unavailable=Function(Graph,Print,950,700);
    if (!Concat || !Success || !Error || !Unavailable) { AddError(TEXT("Primary native Kismet functions missing")); return false; }
    bool Linked=true;
    const auto* Schema=GetDefault<UEdGraphSchema_K2>();
    auto Wire=[&](UEdGraphPin* A,UEdGraphPin* B)
    {
        if (!A || !B || !Schema->TryCreateConnection(A,B))
        { AddError(FString::Printf(TEXT("Illegal demo connection: %s -> %s"),A ? *A->PinName.ToString():TEXT("missing"),B ? *B->PinName.ToString():TEXT("missing")));Linked=false; }
    };
    Concat->FindPinChecked(TEXT("A"))->DefaultValue=TEXT("VAREST_FIRST_LOOK_SUCCESS ");
    auto* ErrorData=Node<UK2Node_BreakStruct>(Graph,180,450);ErrorData->StructType=FVaRestError::StaticStruct();ErrorData->AllocateDefaultPins();
    auto* ErrorText=Function(Graph,UKismetStringLibrary::StaticClass()->FindFunctionByName(
        GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary,Concat_StrStr)),550,450);
    if (!ErrorText) { AddError(TEXT("Primary error-text function missing"));return false; }
    ErrorText->FindPinChecked(TEXT("A"))->DefaultValue=TEXT("VAREST_FIRST_LOOK_ERROR ");
    Wire(Call->FindPin(TEXT("Error")),ErrorData->FindPin(FVaRestError::StaticStruct()->GetFName()));
    Wire(ErrorData->FindPin(TEXT("Message")),ErrorText->FindPin(TEXT("B")));
    Wire(ErrorText->GetReturnValuePin(),Error->FindPin(TEXT("InString")));
    for (auto* PrintNode:{Success,Error,Unavailable}) PrintNode->FindPinChecked(TEXT("Duration"))->DefaultValue=TEXT("60.0");
    Unavailable->FindPinChecked(TEXT("InString"))->DefaultValue=TEXT("VAREST_FIRST_LOOK_TYPED_OUTPUT_UNAVAILABLE");
    Wire(Begin->FindPin(UEdGraphSchema_K2::PN_Then),Call->GetExecPin());
    Wire(Response,StructInput); Wire(Break->FindPin(DisplayName->GetFName()),Concat->FindPin(TEXT("B")));
    Wire(Concat->GetReturnValuePin(),Success->FindPin(TEXT("InString")));
    Wire(Call->FindPin(TEXT("OnSuccess")),Success->GetExecPin());
    Wire(Call->FindPin(TEXT("OnError")),Error->GetExecPin());
    Wire(Call->FindPin(TEXT("OnTypedOutputUnavailable")),Unavailable->GetExecPin());
    if (!Linked) return false;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP.Get()); FCompilerResultsLog Log;
    FKismetEditorUtilities::CompileBlueprint(BP.Get(),EBlueprintCompileOptions::SkipGarbageCollection,&Log);
    if (!TestTrue(TEXT("linked BeginPlay graph compiles"),Log.NumErrors==0 && BP->Status!=BS_Error && BP->GeneratedClass)) return false;
    const auto* Class=Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
    if (!TestTrue(TEXT("actual event bytecode exists"),Class && Class->UberGraphFunction && !Class->UberGraphFunction->Script.IsEmpty())) return false;
    FAssetRegistryModule::AssetCreated(BP.Get()); Package->MarkPackageDirty();
    const FString Filename=FPackageName::LongPackageNameToFilename(BlueprintPath,FPackageName::GetAssetPackageExtension());
    if (!IFileManager::Get().MakeDirectory(*OutputDir,true)) { AddError(TEXT("Cannot create fresh owned output folder"));return false; }
    FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;Args.SaveFlags=SAVE_NoError;
    if (!UPackage::SavePackage(Package.Get(),BP.Get(),*Filename,Args)) { AddError(TEXT("Cannot save owned Blueprint"));return false; }
    UWorld* World=UEditorLoadingAndSavingUtils::NewBlankMap(false);
    if (!TestNotNull(TEXT("actual dedicated blank Editor world"),World)) return false;
    FActorSpawnParameters Spawn;Spawn.Name=TEXT("VaRestFirstLookActor");Spawn.ObjectFlags=RF_Transactional;
    Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AActor* Actor=World->SpawnActor<AActor>(BP->GeneratedClass,FVector(0,0,100),FRotator::ZeroRotator,Spawn);
    if (!TestNotNull(TEXT("real placed Blueprint actor"),Actor)) return false;
    Actor->SetActorLabel(TEXT("VaRest First Look"),false);
    if (!UEditorLoadingAndSavingUtils::SaveMap(World,MapPath)) { AddError(TEXT("Cannot save owned map"));return false; }
    AddInfo(TEXT("VAREST_FIRST_LOOK_AUTHORED Blueprint=/Game/VaRestFirstLook002/BP_NorthwindFirstLook Map=/Game/VaRestFirstLook002/FirstLook; BeginPlay -> real typed Call Endpoint. Runtime/HTTP/PIE still UNRUN."));
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestFirstLookColdLoadTest,"VaRest.Examples.FirstLook.ColdLoad",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
void FVaRestFirstLookColdLoadTest::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
    if (VaRestFirstLookAuthor::Dedicated()) { Names.Add(TEXT("Northwind")); Commands.Add(TEXT("Northwind")); }
}
bool FVaRestFirstLookColdLoadTest::RunTest(const FString& Parameters)
{
    if (!VaRestFirstLookAuthor::Dedicated() || Parameters!=TEXT("Northwind")) return false;
    const TCHAR* ApiPath=TEXT("/Game/FirstLook/DA_NorthwindGameApi.DA_NorthwindGameApi");
    if (FindObject<UVaRestApi>(nullptr,ApiPath))
    { AddError(TEXT("Cold-load precondition failed: API was already loaded; use a fresh sole-test process"));return false; }
    // Load the saved Blueprint first. Never seed its API, types or registry before this load.
    TStrongObjectPtr<UBlueprint> BP(LoadObject<UBlueprint>(nullptr,
        TEXT("/Game/VaRestFirstLook001/BP_NorthwindFirstLook.BP_NorthwindFirstLook")));
    if (!TestNotNull(TEXT("actual saved Blueprint cold-load"),BP.Get())) return false;
    TArray<UK2Node_VaRestCallEndpoint*> Calls;FBlueprintEditorUtils::GetAllNodesOfClass(BP.Get(),Calls);
    if (!TestEqual(TEXT("one saved Call Endpoint"),Calls.Num(),1)) return false;
    auto* Call=Calls[0];
    if (!TestTrue(TEXT("serialized API/GUID resolves without caller preloading"),Call->Api &&
        Call->Api->GetPathName()==ApiPath && Call->EndpointId.IsValid() && Call->ResolveEndpoint() &&
        Call->ResolveEndpoint()->Name==TEXT("GetPlayerProfile"))) return false;
    FCompilerResultsLog Log;FKismetEditorUtilities::CompileBlueprint(BP.Get(),EBlueprintCompileOptions::SkipGarbageCollection,&Log);
    const auto* Class=Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
    return TestTrue(TEXT("cold saved graph recompiles real bytecode without endpoint errors"),Log.NumErrors==0 &&
        BP->Status!=BS_Error && Class && Class->UberGraphFunction && !Class->UberGraphFunction->Script.IsEmpty());
}
#endif
