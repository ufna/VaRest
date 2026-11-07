// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
// Unintegrated opt-in authoring candidate. Runtime output is ordinary Blueprint/components.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "K2Node_VaRestCallEndpoint.h"
#include "VaRestApi.h"
#include "VaRestHttpBlueprintTypes.h"
#include "VaRestHttpTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Camera/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Components/TextRenderComponent.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetGuidLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetTextLibrary.h"
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

namespace VaRestShippingVisualAuthor
{
bool Dedicated()
{
    return FParse::Param(FCommandLine::Get(),TEXT("VaRestShippingVisualFixture"))
        && FParse::Param(FCommandLine::Get(),TEXT("unattended"))
        && FParse::Param(FCommandLine::Get(),TEXT("nowrite"));
}
template<class T> T* Node(UEdGraph* Graph,int32 X,int32 Y)
{
    auto* Out=NewObject<T>(Graph,NAME_None,RF_Transactional);
    Graph->AddNode(Out,false,false);Out->CreateNewGuid();Out->NodePosX=X;Out->NodePosY=Y;return Out;
}
struct FAuthor
{
    FAutomationTestBase& Test;
    UEdGraph* Graph;
    bool Good=true;
    int32 X=0,Y=0;
    void Fail(const FString& Message) { Test.AddError(Message);Good=false; }
    UEdGraphPin* Pin(UEdGraphNode* N,FName Name)
    {
        auto* P=N ? N->FindPin(Name) : nullptr;
        if (!P) Fail(TEXT("Missing genuine graph pin: ")+Name.ToString());
        return P;
    }
    void Wire(UEdGraphPin* A,UEdGraphPin* B)
    {
        if (!A || !B || !GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A,B))
            Fail(FString::Printf(TEXT("Illegal graph link %s -> %s"),A ? *A->PinName.ToString():TEXT("null"),B ? *B->PinName.ToString():TEXT("null")));
    }
    UK2Node_CallFunction* Fn(UFunction* Function)
    {
        if (!Function) { Fail(TEXT("Primary function unavailable"));return nullptr; }
        auto* N=Node<UK2Node_CallFunction>(Graph,X,Y);X+=250;
        N->SetFromFunction(Function);N->AllocateDefaultPins();return N;
    }
    UK2Node_VariableGet* Get(FName Name)
    {
        auto* N=Node<UK2Node_VariableGet>(Graph,X,Y+180);X+=180;
        N->VariableReference.SetSelfMember(Name);N->AllocateDefaultPins();return N;
    }
    UEdGraphPin* Field(UK2Node_BreakStruct* N,UScriptStruct* Type,const TCHAR* Name)
    {
        FProperty* Found=nullptr;
        for (TFieldIterator<FProperty> It(Type);It;++It)
            if (It->GetAuthoredName().Equals(Name,ESearchCase::CaseSensitive))
            { if (Found) { Fail(TEXT("Ambiguous authored field"));return nullptr; } Found=*It; }
        if (!Found) { Fail(FString(TEXT("Missing authored field: "))+Name);return nullptr; }
        return Pin(N,Found->GetFName());
    }
    UEdGraphPin* StructPin(UEdGraphNode* N,UScriptStruct* Type,EEdGraphPinDirection Direction)
    {
        if (!N || !Type) { Fail(TEXT("Missing actual node/struct"));return nullptr; }
        UEdGraphPin* Found=nullptr;
        for (auto* P:N->Pins) if (P->Direction==Direction && P->PinType.PinCategory==UEdGraphSchema_K2::PC_Struct && P->PinType.PinSubCategoryObject.Get()==Type)
        { if (Found) { Fail(TEXT("Ambiguous actual struct pin"));return nullptr; } Found=P; }
        if (!Found) Fail(TEXT("Actual struct pin unavailable"));
        return Found;
    }
    UEdGraphPin* String(UEdGraphPin* Value,UFunction* Convert=nullptr,FName Input=NAME_None)
    {
        if (!Convert) return Value;
        auto* C=Fn(Convert);Wire(Value,Pin(C,Input));return C ? C->GetReturnValuePin():nullptr;
    }
    UEdGraphPin* Prefix(const TCHAR* Label,UEdGraphPin* Value)
    {
        auto* N=Fn(UKismetStringLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary,Concat_StrStr)));
        if (auto* P=Pin(N,TEXT("A"))) P->DefaultValue=Label;
        Wire(Value,Pin(N,TEXT("B")));return N ? N->GetReturnValuePin():nullptr;
    }
    UEdGraphPin* SetText(UEdGraphPin* Exec,FName Component,UEdGraphPin* Value,const TCHAR* Literal=nullptr)
    {
        auto* Text=Fn(UKismetTextLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetTextLibrary,Conv_StringToText)));
        if (Literal) { if (auto* P=Pin(Text,TEXT("InString"))) P->DefaultValue=Literal; }
        else Wire(Value,Pin(Text,TEXT("InString")));
        auto* Target=Get(Component);
        auto* Set=Fn(UTextRenderComponent::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UTextRenderComponent,K2_SetText)));
        Wire(Pin(Target,Component),Pin(Set,UEdGraphSchema_K2::PN_Self));
        Wire(Text ? Text->GetReturnValuePin():nullptr,Pin(Set,TEXT("Value")));
        Wire(Exec,Set ? Set->GetExecPin():nullptr);return Pin(Set,UEdGraphSchema_K2::PN_Then);
    }
};
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestShippingVisualAuthorTest,"VaRest.Examples.ShippingVisual.Author",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
void FVaRestShippingVisualAuthorTest::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
    if (VaRestShippingVisualAuthor::Dedicated()) { Names.Add(TEXT("Northwind"));Commands.Add(TEXT("Northwind")); }
}
bool FVaRestShippingVisualAuthorTest::RunTest(const FString& Parameters)
{
    using namespace VaRestShippingVisualAuthor;
    if (!Dedicated() || Parameters!=TEXT("Northwind")) { AddError(TEXT("Dedicated sole nowrite authoring process required"));return false; }
    const FString Folder=TEXT("/Game/VaRestFirstLookShipping004");
    const FString BlueprintPath=Folder+TEXT("/BP_NorthwindVisual");
    const FString OutputDir=FPaths::ProjectContentDir()/TEXT("VaRestFirstLookShipping004");
    if (IFileManager::Get().DirectoryExists(*OutputDir)) { AddError(TEXT("Output folder occupied; no overwrite"));return false; }
    for (const auto& Path:{BlueprintPath,Folder+TEXT("/FirstLook"),Folder+TEXT("/FirstLookAuto")})
        if (FPackageName::DoesPackageExist(Path) || FindPackage(nullptr,*Path)) { AddError(TEXT("Output package occupied"));return false; }
    TStrongObjectPtr<UVaRestApi> Api(LoadObject<UVaRestApi>(nullptr,TEXT("/Game/FirstLook/DA_NorthwindGameApi.DA_NorthwindGameApi")));
    if (!TestNotNull(TEXT("actual saved Northwind API"),Api.Get())) return false;
    const FVaRestApiEndpoint *ProfileEndpoint=nullptr,*MatchEndpoint=nullptr;
    for (const auto& E:Api->Endpoints)
    {
        if (E.Name==TEXT("GetPlayerProfile")) { if (ProfileEndpoint) return false;ProfileEndpoint=&E; }
        if (E.Name==TEXT("CreateMatch")) { if (MatchEndpoint) return false;MatchEndpoint=&E; }
    }
    if (!TestTrue(TEXT("actual GET/profile and POST/matches GUIDs"),ProfileEndpoint && MatchEndpoint &&
        ProfileEndpoint->Id.IsValid() && MatchEndpoint->Id.IsValid() && ProfileEndpoint->Verb.Equals(TEXT("GET"),ESearchCase::CaseSensitive) &&
        ProfileEndpoint->Path.Equals(TEXT("/profile"),ESearchCase::CaseSensitive) && MatchEndpoint->Verb.Equals(TEXT("POST"),ESearchCase::CaseSensitive) && MatchEndpoint->Path.Equals(TEXT("/matches"),ESearchCase::CaseSensitive))) return false;
    TStrongObjectPtr<UPackage> Package(CreatePackage(*BlueprintPath));
    TStrongObjectPtr<UBlueprint> BP(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),Package.Get(),
        TEXT("BP_NorthwindVisual"),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass()));
    if (!TestTrue(TEXT("normal Actor/SCS"),BP && BP->SimpleConstructionScript)) return false;
    BP->SetFlags(RF_Public|RF_Standalone|RF_Transactional);
    FEdGraphPinType BoolType;BoolType.PinCategory=UEdGraphSchema_K2::PC_Boolean;
    if (!FBlueprintEditorUtils::AddMemberVariable(BP.Get(),TEXT("AutoCloseAfterDemo"),BoolType,TEXT("false"))) return false;
    FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(BP.Get(),TEXT("AutoCloseAfterDemo"),false);
    auto* SCS=BP->SimpleConstructionScript.Get();
    USCS_Node* Root=nullptr;SCS->GetSceneRootComponentTemplate(true,&Root);
    if (!Root) { Root=SCS->CreateNode(USceneComponent::StaticClass(),TEXT("DemoRoot"));SCS->AddNode(Root); }
    auto* CameraNode=SCS->CreateNode(UCameraComponent::StaticClass(),TEXT("DemoCamera"));Root->AddChildNode(CameraNode);
    auto* Camera=Cast<UCameraComponent>(CameraNode->ComponentTemplate);
    if (!TestNotNull(TEXT("native camera template"),Camera)) return false;
    Camera->SetAutoActivate(true);Camera->SetFieldOfView(80.f);Camera->SetConstraintAspectRatio(false);
    auto AddText=[&](FName Name,const TCHAR* Initial,float Height,float Size,FColor Color) {
        auto* N=SCS->CreateNode(UTextRenderComponent::StaticClass(),Name);Root->AddChildNode(N);
        auto* T=Cast<UTextRenderComponent>(N->ComponentTemplate);if (!T) return false;
        T->SetRelativeLocation(FVector(500,0,Height));T->SetRelativeRotation(FRotator(0,180,0));
        T->SetHorizontalAlignment(EHTA_Center);T->SetWorldSize(Size);T->SetTextRenderColor(Color);T->SetText(FText::FromString(Initial));return true;
    };
    if (!AddText(TEXT("Title"),TEXT("VaRest 2.0 | Northwind"),170,34,FColor(110,230,200)) ||
        !AddText(TEXT("Status"),TEXT("Requesting profile..."),110,22,FColor::White) ||
        !AddText(TEXT("Profile"),TEXT("Profile: waiting"),50,25,FColor::White) ||
        !AddText(TEXT("ProfileHttp"),TEXT("HTTP profile: waiting"),10,18,FColor::White) ||
        !AddText(TEXT("Match"),TEXT("Match: waiting"),-40,18,FColor::White) ||
        !AddText(TEXT("Players"),TEXT("Players: waiting"),-80,18,FColor::White) ||
        !AddText(TEXT("MatchHttp"),TEXT("HTTP match: waiting"),-120,18,FColor::White)) return false;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP.Get());FCompilerResultsLog Initial;
    FKismetEditorUtilities::CompileBlueprint(BP.Get(),EBlueprintCompileOptions::SkipGarbageCollection,&Initial);
    if (!TestTrue(TEXT("SCS/member properties compile"),Initial.NumErrors==0 && BP->GeneratedClass)) return false;
    UEdGraph* Graph=FBlueprintEditorUtils::FindEventGraph(BP.Get());if (!Graph) return false;
    FAuthor A{*this,Graph};
    TArray<UK2Node_Event*> Events;FBlueprintEditorUtils::GetAllNodesOfClass(BP.Get(),Events);UK2Node_Event* Begin=nullptr;
    for (auto* E:Events) if (E->EventReference.GetMemberName()==TEXT("ReceiveBeginPlay")) { if (Begin) return false;Begin=E; }
    if (!Begin) { Begin=Node<UK2Node_Event>(Graph,-1200,0);Begin->EventReference.SetExternalMember(TEXT("ReceiveBeginPlay"),AActor::StaticClass());Begin->bOverrideFunction=true;Begin->AllocateDefaultPins(); }
    auto* Self=Node<UK2Node_Self>(Graph,-1100,220);Self->AllocateDefaultPins();
    auto* PC=A.Fn(UGameplayStatics::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UGameplayStatics,GetPlayerController)));
    auto* View=A.Fn(APlayerController::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(APlayerController,SetViewTargetWithBlend)));
    A.Wire(PC ? PC->GetReturnValuePin():nullptr,A.Pin(View,UEdGraphSchema_K2::PN_Self));
    A.Wire(A.Pin(Self,UEdGraphSchema_K2::PN_Self),A.Pin(View,TEXT("NewViewTarget")));
    A.Wire(A.Pin(Begin,UEdGraphSchema_K2::PN_Then),View ? View->GetExecPin():nullptr);
    auto Call=[&](const FVaRestApiEndpoint* E,int32 X,int32 Y) {
        auto* N=Node<UK2Node_VaRestCallEndpoint>(Graph,X,Y);N->Api=Api.Get();N->EndpointId=E->Id;N->EndpointName=E->Name;N->AllocateDefaultPins();return N;
    };
    auto* ProfileCall=Call(ProfileEndpoint,700,0);auto* MatchCall=Call(MatchEndpoint,2600,0);
    A.Wire(A.Pin(View,UEdGraphSchema_K2::PN_Then),ProfileCall->GetExecPin());
    auto Type=[&](UEdGraphPin* P) { return P ? Cast<UScriptStruct>(P->PinType.PinSubCategoryObject.Get()):nullptr; };
    auto* Profile=Type(A.Pin(ProfileCall,TEXT("Response")));auto* Request=Type(A.Pin(MatchCall,TEXT("Body")));auto* Match=Type(A.Pin(MatchCall,TEXT("Response")));
    if (!TestTrue(TEXT("three genuine generated structs"),Profile && Request && Match)) return false;
    auto* BreakProfile=Node<UK2Node_BreakStruct>(Graph,700,300);BreakProfile->StructType=Profile;BreakProfile->AllocateDefaultPins();
    A.Wire(A.Pin(ProfileCall,TEXT("Response")),A.StructPin(BreakProfile,Profile,EGPD_Input));
    auto* Make=Node<UK2Node_MakeStruct>(Graph,2250,300);Make->StructType=Request;Make->AllocateDefaultPins();
    auto RequestField=[&](const TCHAR* Name) -> UEdGraphPin* {
        FProperty* P=nullptr;for (TFieldIterator<FProperty> It(Request);It;++It) if (It->GetAuthoredName().Equals(Name,ESearchCase::CaseSensitive)) { if (P) return nullptr;P=*It; }
        return P ? A.Pin(Make,P->GetFName()):nullptr;
    };
    // A genuine decoded value drives a second typed request, rather than a constant proof.
    A.Wire(A.Field(BreakProfile,Profile,TEXT("displayName")),RequestField(TEXT("map")));
    A.Wire(A.Field(BreakProfile,Profile,TEXT("level")),RequestField(TEXT("maxPlayers")));
    A.Wire(A.Field(BreakProfile,Profile,TEXT("online")),RequestField(TEXT("ranked")));
    A.Wire(A.StructPin(Make,Request,EGPD_Output),A.Pin(MatchCall,TEXT("Body")));
    A.X=1100;A.Y=0;
    auto* Exec=A.SetText(A.Pin(ProfileCall,TEXT("OnSuccess")),TEXT("Profile"),A.Prefix(TEXT("Profile: "),A.Field(BreakProfile,Profile,TEXT("displayName"))));
    auto Http=[&](UK2Node_VaRestCallEndpoint* C) {
        auto* N=Node<UK2Node_BreakStruct>(Graph,A.X,A.Y+450);N->StructType=FVaRestResponseData::StaticStruct();N->AllocateDefaultPins();
        A.Wire(A.Pin(C,TEXT("Http")),A.StructPin(N,FVaRestResponseData::StaticStruct(),EGPD_Input));
        return A.String(A.Pin(N,TEXT("Status")),UKismetStringLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary,Conv_IntToString)),TEXT("InInt"));
    };
    Exec=A.SetText(Exec,TEXT("ProfileHttp"),A.Prefix(TEXT("HTTP profile: "),Http(ProfileCall)));
    A.Wire(Exec,MatchCall->GetExecPin());
    auto* BreakMatch=Node<UK2Node_BreakStruct>(Graph,2900,300);BreakMatch->StructType=Match;BreakMatch->AllocateDefaultPins();
    A.Wire(A.Pin(MatchCall,TEXT("Response")),A.StructPin(BreakMatch,Match,EGPD_Input));
    auto* Id=A.Field(BreakMatch,Match,TEXT("id"));UEdGraphPin* IdString=Id;
    if (Id && Id->PinType.PinCategory==UEdGraphSchema_K2::PC_Struct && Id->PinType.PinSubCategoryObject.Get()==TBaseStructure<FGuid>::Get())
        IdString=A.String(Id,UKismetGuidLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetGuidLibrary,Conv_GuidToString)),TEXT("InGuid"));
    else if (!Id || Id->PinType.PinCategory!=UEdGraphSchema_K2::PC_String) { AddError(TEXT("Unsupported genuine Match.id type"));return false; }
    A.X=3100;A.Y=0;
    Exec=A.SetText(A.Pin(MatchCall,TEXT("OnSuccess")),TEXT("Match"),A.Prefix(TEXT("Match: "),IdString));
    auto* Players=A.String(A.Field(BreakMatch,Match,TEXT("players")),UKismetStringLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary,Conv_IntToString)),TEXT("InInt"));
    Exec=A.SetText(Exec,TEXT("Players"),A.Prefix(TEXT("Players: "),Players));
    Exec=A.SetText(Exec,TEXT("MatchHttp"),A.Prefix(TEXT("HTTP match: "),Http(MatchCall)));
    Exec=A.SetText(Exec,TEXT("Status"),nullptr,TEXT("Completed: typed response -> typed request"));
    auto* Close=Node<UK2Node_IfThenElse>(Graph,A.X,0);Close->AllocateDefaultPins();
    // Camera selection after the real async responses also survives ordinary
    // controller startup camera management. This does not delay either request.
    auto* ResultView=A.Fn(APlayerController::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(APlayerController,SetViewTargetWithBlend)));
    A.Wire(PC ? PC->GetReturnValuePin():nullptr,A.Pin(ResultView,UEdGraphSchema_K2::PN_Self));
    A.Wire(A.Pin(Self,UEdGraphSchema_K2::PN_Self),A.Pin(ResultView,TEXT("NewViewTarget")));
    A.Wire(Exec,ResultView ? ResultView->GetExecPin():nullptr);
    A.Wire(A.Pin(ResultView,UEdGraphSchema_K2::PN_Then),Close->GetExecPin());auto* Auto=A.Get(TEXT("AutoCloseAfterDemo"));A.Wire(A.Pin(Auto,TEXT("AutoCloseAfterDemo")),Close->GetConditionPin());
    auto* Delay=A.Fn(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary,Delay)));
    if (auto* P=A.Pin(Delay,TEXT("Duration"))) P->DefaultValue=TEXT("10.0");
    auto* Quit=A.Fn(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary,QuitGame)));
    A.Wire(A.Pin(Close,UEdGraphSchema_K2::PN_Then),Delay ? Delay->GetExecPin():nullptr);
    A.Wire(A.Pin(Delay,UEdGraphSchema_K2::PN_Then),Quit ? Quit->GetExecPin():nullptr);
    A.Wire(PC ? PC->GetReturnValuePin():nullptr,A.Pin(Quit,TEXT("SpecificPlayer")));
    // Delay belongs only to fixture close AFTER a completed/error response. There is no startup/request delay.
    for (auto* CallNode:{ProfileCall,MatchCall})
    {
        A.Y+=900;A.X=1200;
        auto* Error=Node<UK2Node_BreakStruct>(Graph,A.X,A.Y+300);Error->StructType=FVaRestError::StaticStruct();Error->AllocateDefaultPins();
        A.Wire(A.Pin(CallNode,TEXT("Error")),A.StructPin(Error,FVaRestError::StaticStruct(),EGPD_Input));
        auto* ErrorExec=A.SetText(A.Pin(CallNode,TEXT("OnError")),TEXT("Status"),A.Prefix(TEXT("Error: "),A.Pin(Error,TEXT("Message"))));
        A.Wire(ErrorExec,Close->GetExecPin());
        auto* Unavailable=A.SetText(A.Pin(CallNode,TEXT("OnTypedOutputUnavailable")),TEXT("Status"),nullptr,TEXT("Typed output unavailable"));
        A.Wire(Unavailable,Close->GetExecPin());
    }
    if (!A.Good) return false;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP.Get());FCompilerResultsLog Log;
    FKismetEditorUtilities::CompileBlueprint(BP.Get(),EBlueprintCompileOptions::SkipGarbageCollection,&Log);
    auto* Class=Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
    if (!TestTrue(TEXT("actual connected graph bytecode"),Log.NumErrors==0 && BP->Status!=BS_Error && Class && Class->UberGraphFunction && !Class->UberGraphFunction->Script.IsEmpty())) return false;
    auto* AutoProperty=FindFProperty<FBoolProperty>(Class,TEXT("AutoCloseAfterDemo"));
    if (!TestNotNull(TEXT("real default-false auto-close member"),AutoProperty) || AutoProperty->GetPropertyValue_InContainer(Class->GetDefaultObject())) return false;
    FAssetRegistryModule::AssetCreated(BP.Get());Package->MarkPackageDirty();
    if (!IFileManager::Get().MakeDirectory(*OutputDir,true)) return false;
    FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;Args.SaveFlags=SAVE_NoError;
    if (!UPackage::SavePackage(Package.Get(),BP.Get(),*FPackageName::LongPackageNameToFilename(BlueprintPath,FPackageName::GetAssetPackageExtension()),Args)) return false;
    for (const bool bAuto:{false,true})
    {
        UWorld* World=UEditorLoadingAndSavingUtils::NewBlankMap(false);if (!World) return false;
        FActorSpawnParameters Spawn;Spawn.Name=TEXT("VaRestVisualDemoActor");Spawn.ObjectFlags=RF_Transactional;
        Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* Actor=World->SpawnActor<AActor>(Class,FVector::ZeroVector,FRotator::ZeroRotator,Spawn);if (!Actor) return false;
        AutoProperty->SetPropertyValue_InContainer(Actor,bAuto);Actor->RerunConstructionScripts();
        if (!TestTrue(TEXT("per-instance close setting survives normal reconstruction"),AutoProperty->GetPropertyValue_InContainer(Actor)==bAuto)) return false;
        Actor->SetActorLabel(TEXT("VaRest typed HTTP visual demo"),false);
        TArray<UTextRenderComponent*> Texts;Actor->GetComponents(Texts);
        if (!TestTrue(TEXT("real camera and text runtime components"),Actor->FindComponentByClass<UCameraComponent>() && Texts.Num()==7)) return false;
        if (!UEditorLoadingAndSavingUtils::SaveMap(World,Folder+(bAuto ? TEXT("/FirstLookAuto"):TEXT("/FirstLook")))) return false;
    }
    AddInfo(TEXT("VAREST_SHIPPING_VISUAL_AUTHORED: normal Blueprint/camera/text; GET profile drives POST typed Body; actual runtime/Shipping proof remains UNRUN."));
    return true;
}
#endif
