// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
// Opt-in authoring fixture. Runtime output uses ordinary Blueprint/components.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestHttpTypes.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Camera/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/TextRenderComponent.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableSet.h"
#include "K2Node_BreakStruct.h"
#include "VaRestSocketComponent.h"
#include "VaRestSocketLibrary.h"
#include "VaRestSocketClient.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_EnumEquality.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetGuidLibrary.h"
#include "Kismet/KismetMathLibrary.h"
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

namespace VaRestSocketVisualAuthor
{
bool Dedicated()
{
    return FParse::Param(FCommandLine::Get(),TEXT("VaRestSocketShippingFixture"))
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


IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestSocketShippingAuthorTest,"VaRest.Examples.SocketShipping.Author",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
void FVaRestSocketShippingAuthorTest::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
    if (VaRestSocketVisualAuthor::Dedicated()) { Names.Add(TEXT("PublicEcho"));Commands.Add(TEXT("PublicEcho"));Names.Add(TEXT("Cold"));Commands.Add(TEXT("Cold")); }
}
bool FVaRestSocketShippingAuthorTest::RunTest(const FString& Parameters)
{
    using namespace VaRestSocketVisualAuthor;
    if (!Dedicated() || (Parameters!=TEXT("PublicEcho") && Parameters!=TEXT("Cold"))) { AddError(TEXT("Dedicated sole nowrite socket author required"));return false; }
    const FString Folder=TEXT("/Game/VaRestFirstLookSocket007");
    const FString BlueprintPath=Folder+TEXT("/BP_PublicSocket");
    const FString OutputDir=FPaths::ProjectContentDir()/TEXT("VaRestFirstLookSocket007");
    if (Parameters==TEXT("Cold"))
    {
        TStrongObjectPtr<UBlueprint> Saved(LoadObject<UBlueprint>(nullptr,*(BlueprintPath+TEXT(".BP_PublicSocket"))));
        auto* Class=Saved ? Cast<UBlueprintGeneratedClass>(Saved->GeneratedClass):nullptr;
        if (!TestTrue(TEXT("cold genuine saved connected graph"),Class && Class->UberGraphFunction && !Class->UberGraphFunction->Script.IsEmpty())) return false;
        for (const FName Event:{FName(TEXT("SocketReady")),FName(TEXT("SocketMessage")),FName(TEXT("SocketClosed")),FName(TEXT("SocketError")),FName(TEXT("SocketGaveUp"))})
            if (!TestNotNull(TEXT("cold actual delegate event function"),Class->FindFunctionByName(Event))) return false;
        auto* Auto=FindFProperty<FBoolProperty>(Class,TEXT("AutoCloseAfterDemo"));
        auto* Matched=FindFProperty<FBoolProperty>(Class,TEXT("EchoMatched"));if (!Auto || !Matched) return false;
        for (const bool bAuto:{false,true})
        {
            const FString Name=bAuto ? TEXT("FirstLookAuto"):TEXT("FirstLook");
            TStrongObjectPtr<UWorld> World(LoadObject<UWorld>(nullptr,*(Folder+TEXT("/")+Name+TEXT(".")+Name)));
            if (!World || !World->PersistentLevel) return false;
            AActor* Found=nullptr;
            for (AActor* Actor:World->PersistentLevel->Actors)
                if (Actor && Actor->GetClass()==Class) { if (Found) { AddError(TEXT("Duplicate saved demo actor"));return false; } Found=Actor; }
            if (!TestNotNull(TEXT("cold exact class actor in actual saved level"),Found)) return false;
            auto* Component=Found->FindComponentByClass<UVaRestSocketComponent>();TArray<UTextRenderComponent*> Texts;Found->GetComponents(Texts);
            if (!TestTrue(TEXT("cold saved public component/fields"),Component && Component->Url==TEXT("ws://localhost:18445/echo") && !Component->bAutoConnect && Component->bCloseOnEndPlay && Component->Scope==EVaRestSocketScope::Actor && Texts.Num()==7 && Found->FindComponentByClass<UCameraComponent>() && Found->FindComponentByClass<UDirectionalLightComponent>() && Auto->GetPropertyValue_InContainer(Found)==bAuto && !Matched->GetPropertyValue_InContainer(Found))) return false;
        }
        AddInfo(TEXT("VAREST_SOCKET_SHIPPING_COLD: actual saved normal Blueprint/delegate functions/both maps/component properties; no BeginPlay/network/author writes executed."));return true;
    }
    if (IFileManager::Get().DirectoryExists(*OutputDir)) { AddError(TEXT("Output folder occupied; no overwrite"));return false; }
    for (const auto& Path:{BlueprintPath,Folder+TEXT("/FirstLook"),Folder+TEXT("/FirstLookAuto")})
        if (FPackageName::DoesPackageExist(Path) || FindPackage(nullptr,*Path)) { AddError(TEXT("Output package occupied"));return false; }
    constexpr const TCHAR* Marker=TEXT("\"VaRest cooked WebSocket demo\"");
    TStrongObjectPtr<UPackage> Package(CreatePackage(*BlueprintPath));
    TStrongObjectPtr<UBlueprint> BP(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),Package.Get(),
        TEXT("BP_PublicSocket"),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass()));
    if (!TestTrue(TEXT("normal Actor/SCS"),BP && BP->SimpleConstructionScript)) return false;
    BP->SetFlags(RF_Public|RF_Standalone|RF_Transactional);
    FEdGraphPinType BoolType;BoolType.PinCategory=UEdGraphSchema_K2::PC_Boolean;
    for (FName Member:{FName(TEXT("AutoCloseAfterDemo")),FName(TEXT("EchoMatched"))})
        if (!FBlueprintEditorUtils::AddMemberVariable(BP.Get(),Member,BoolType,TEXT("false"))) return false;
    FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(BP.Get(),TEXT("AutoCloseAfterDemo"),false);
    auto* SCS=BP->SimpleConstructionScript.Get();USCS_Node* Root=nullptr;SCS->GetSceneRootComponentTemplate(true,&Root);
    if (!Root) { Root=SCS->CreateNode(USceneComponent::StaticClass(),TEXT("DemoRoot"));SCS->AddNode(Root); }
    auto* CameraNode=SCS->CreateNode(UCameraComponent::StaticClass(),TEXT("DemoCamera"));Root->AddChildNode(CameraNode);
    auto* Camera=Cast<UCameraComponent>(CameraNode->ComponentTemplate);if (!TestNotNull(TEXT("native camera"),Camera)) return false;
    Camera->SetAutoActivate(true);Camera->SetFieldOfView(80.f);Camera->SetConstraintAspectRatio(false);
    auto* LightNode=SCS->CreateNode(UDirectionalLightComponent::StaticClass(),TEXT("DemoLight"));Root->AddChildNode(LightNode);
    auto* Light=Cast<UDirectionalLightComponent>(LightNode->ComponentTemplate);if (!Light) return false;
    Light->SetMobility(EComponentMobility::Movable);Light->SetIntensity(5.f);Light->SetCastShadows(false);
    auto* SocketNode=SCS->CreateNode(UVaRestSocketComponent::StaticClass(),TEXT("PublicSocket"));SCS->AddNode(SocketNode);
    auto* Socket=Cast<UVaRestSocketComponent>(SocketNode->ComponentTemplate);if (!Socket) return false;
    Socket->Url=TEXT("ws://localhost:18445/echo");Socket->Scope=EVaRestSocketScope::Actor;
    Socket->bAutoConnect=false;Socket->bCloseOnEndPlay=true;
    auto AddText=[&](FName Name,const TCHAR* Initial,float Height,float Size,FColor Color) {
        auto* N=SCS->CreateNode(UTextRenderComponent::StaticClass(),Name);Root->AddChildNode(N);
        auto* T=Cast<UTextRenderComponent>(N->ComponentTemplate);if (!T) return false;
        T->SetRelativeLocation(FVector(500,0,Height));T->SetRelativeRotation(FRotator(0,180,0));
        T->SetHorizontalAlignment(EHTA_Center);T->SetWorldSize(Size);T->SetTextRenderColor(Color);T->SetText(FText::FromString(Initial));return true;
    };
    if (!AddText(TEXT("Title"),TEXT("VaRest 2.0 | WebSocket"),170,34,FColor(110,230,200)) ||
        !AddText(TEXT("Status"),TEXT("Connecting with public component..."),110,22,FColor::White) ||
        !AddText(TEXT("Endpoint"),TEXT("ws://localhost:18445/echo"),60,22,FColor::White) ||
        !AddText(TEXT("Requested"),TEXT("Requested: waiting"),10,19,FColor::White) ||
        !AddText(TEXT("Echo"),TEXT("Echo: waiting"),-40,19,FColor::White) ||
        !AddText(TEXT("Close"),TEXT("Close: waiting"),-90,19,FColor::White) ||
        !AddText(TEXT("Route"),TEXT("Ordinary Blueprint / public SocketComponent"),-140,17,FColor::White)) return false;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP.Get());FCompilerResultsLog Initial;
    FKismetEditorUtilities::CompileBlueprint(BP.Get(),EBlueprintCompileOptions::SkipGarbageCollection,&Initial);
    if (!TestTrue(TEXT("normal runtime component/member properties compile"),Initial.NumErrors==0 && BP->GeneratedClass)) return false;
    UEdGraph* Graph=FBlueprintEditorUtils::FindEventGraph(BP.Get());if (!Graph) return false;FAuthor A{*this,Graph};
    TArray<UK2Node_Event*> Events;FBlueprintEditorUtils::GetAllNodesOfClass(BP.Get(),Events);UK2Node_Event* Begin=nullptr;
    for (auto* E:Events) if (E->EventReference.GetMemberName()==TEXT("ReceiveBeginPlay")) { if (Begin) return false;Begin=E; }
    if (!Begin) { Begin=Node<UK2Node_Event>(Graph,-1200,0);Begin->EventReference.SetExternalMember(TEXT("ReceiveBeginPlay"),AActor::StaticClass());Begin->bOverrideFunction=true;Begin->AllocateDefaultPins(); }
    auto* Self=Node<UK2Node_Self>(Graph,-1200,180);Self->AllocateDefaultPins();
    auto* PC=A.Fn(UGameplayStatics::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UGameplayStatics,GetPlayerController)));
    auto* View=A.Fn(APlayerController::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(APlayerController,SetViewTargetWithBlend)));
    A.Wire(PC ? PC->GetReturnValuePin():nullptr,A.Pin(View,UEdGraphSchema_K2::PN_Self));
    A.Wire(A.Pin(Self,UEdGraphSchema_K2::PN_Self),A.Pin(View,TEXT("NewViewTarget")));
    A.Wire(A.Pin(Begin,UEdGraphSchema_K2::PN_Then),View ? View->GetExecPin():nullptr);
    auto* Component=A.Get(TEXT("PublicSocket"));
    auto* GetClient=A.Fn(UVaRestSocketComponent::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UVaRestSocketComponent,GetClient)));
    A.Wire(A.Pin(Component,TEXT("PublicSocket")),A.Pin(GetClient,UEdGraphSchema_K2::PN_Self));
    auto* Client=GetClient ? GetClient->GetReturnValuePin():nullptr;
    auto ClientCall=[&](FName Name) {
        auto* Call=A.Fn(UVaRestSocketClient::StaticClass()->FindFunctionByName(Name));
        A.Wire(Client,A.Pin(Call,UEdGraphSchema_K2::PN_Self));return Call;
    };
    auto* Auto=A.Get(TEXT("AutoCloseAfterDemo"));
    auto* QuitBranch=Node<UK2Node_IfThenElse>(Graph,6500,1800);QuitBranch->AllocateDefaultPins();
    A.Wire(A.Pin(Auto,TEXT("AutoCloseAfterDemo")),QuitBranch->GetConditionPin());
    auto* Delay=A.Fn(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary,Delay)));
    if (auto* P=A.Pin(Delay,TEXT("Duration"))) P->DefaultValue=TEXT("10.0");
    auto* Quit=A.Fn(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary,QuitGame)));
    A.Wire(A.Pin(QuitBranch,UEdGraphSchema_K2::PN_Then),Delay ? Delay->GetExecPin():nullptr);
    A.Wire(A.Pin(Delay,UEdGraphSchema_K2::PN_Then),Quit ? Quit->GetExecPin():nullptr);
    A.Wire(PC ? PC->GetReturnValuePin():nullptr,A.Pin(Quit,TEXT("SpecificPlayer")));
    auto ErrorTail=[&](UEdGraphPin* Exec,const TCHAR* Message) {
        auto* Tail=A.SetText(Exec,TEXT("Status"),nullptr,Message);A.Wire(Tail,QuitBranch->GetExecPin());
    };
    auto* Valid=A.Fn(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary,IsValid)));
    A.Wire(Client,A.Pin(Valid,TEXT("Object")));
    auto* ValidBranch=Node<UK2Node_IfThenElse>(Graph,0,0);ValidBranch->AllocateDefaultPins();
    A.Wire(A.Pin(View,UEdGraphSchema_K2::PN_Then),ValidBranch->GetExecPin());A.Wire(Valid ? Valid->GetReturnValuePin():nullptr,ValidBranch->GetConditionPin());
    ErrorTail(A.Pin(ValidBranch,UEdGraphSchema_K2::PN_Else),TEXT("Error: public socket client unavailable"));
    auto* Exec=A.Pin(ValidBranch,UEdGraphSchema_K2::PN_Then);
    auto Bind=[&](FName PropertyName,const TCHAR* EventName) -> UK2Node_CustomEvent* {
        auto* Property=FindFProperty<FMulticastDelegateProperty>(UVaRestSocketClient::StaticClass(),PropertyName);
        if (!Property || !Property->SignatureFunction) { A.Fail(TEXT("Actual public delegate unavailable"));return nullptr; }
        auto* Event=UK2Node_CustomEvent::CreateFromFunction(FVector2D(A.X,A.Y+1400),Graph,EventName,Property->SignatureFunction,false);
        auto* Add=Node<UK2Node_AddDelegate>(Graph,A.X,A.Y);Add->SetFromProperty(Property,false,UVaRestSocketClient::StaticClass());Add->AllocateDefaultPins();
        A.Wire(Client,A.Pin(Add,UEdGraphSchema_K2::PN_Self));A.Wire(Event ? Event->GetDelegatePin():nullptr,Add->GetDelegatePin());
        A.Wire(Exec,A.Pin(Add,UEdGraphSchema_K2::PN_Execute));Exec=A.Pin(Add,UEdGraphSchema_K2::PN_Then);A.X+=300;return Event;
    };
    auto* Ready=Bind(GET_MEMBER_NAME_CHECKED(UVaRestSocketClient,OnReady),TEXT("SocketReady"));
    auto* Message=Bind(GET_MEMBER_NAME_CHECKED(UVaRestSocketClient,OnMessage),TEXT("SocketMessage"));
    auto* Closed=Bind(GET_MEMBER_NAME_CHECKED(UVaRestSocketClient,OnDisconnected),TEXT("SocketClosed"));
    auto* Error=Bind(GET_MEMBER_NAME_CHECKED(UVaRestSocketClient,OnError),TEXT("SocketError"));
    auto* GaveUp=Bind(GET_MEMBER_NAME_CHECKED(UVaRestSocketClient,OnGaveUp),TEXT("SocketGaveUp"));
    auto* MakePolicy=Node<UK2Node_MakeStruct>(Graph,A.X,300);MakePolicy->StructType=FVaRestReconnectPolicy::StaticStruct();MakePolicy->AllocateDefaultPins();
    if (auto* P=A.Pin(MakePolicy,GET_MEMBER_NAME_CHECKED(FVaRestReconnectPolicy,bEnabled))) P->DefaultValue=TEXT("false");
    auto* Policy=ClientCall(GET_FUNCTION_NAME_CHECKED(UVaRestSocketClient,SetReconnectPolicy));
    A.Wire(A.StructPin(MakePolicy,FVaRestReconnectPolicy::StaticStruct(),EGPD_Output),A.Pin(Policy,TEXT("Policy")));
    A.Wire(Exec,Policy ? Policy->GetExecPin():nullptr);
    auto* Connect=ClientCall(GET_FUNCTION_NAME_CHECKED(UVaRestSocketClient,Connect));
    A.Wire(A.Pin(Policy,UEdGraphSchema_K2::PN_Then),Connect ? Connect->GetExecPin():nullptr);
    A.X=900;A.Y=1800;
    auto* Sent=A.SetText(A.Pin(Ready,UEdGraphSchema_K2::PN_Then),TEXT("Status"),nullptr,TEXT("Connected: sending public text"));
    auto* Send=ClientCall(GET_FUNCTION_NAME_CHECKED(UVaRestSocketClient,SendText));if (auto* P=A.Pin(Send,TEXT("Text"))) P->DefaultValue=Marker;
    A.Wire(Sent,Send ? Send->GetExecPin():nullptr);
    A.SetText(A.Pin(Send,UEdGraphSchema_K2::PN_Then),TEXT("Requested"),nullptr,TEXT("Requested: \"VaRest cooked WebSocket demo\""));
    A.X=900;A.Y=3000;
    auto* Text=A.Fn(UVaRestSocketLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UVaRestSocketLibrary,SocketMessageText)));
    A.Wire(A.Pin(Message,TEXT("Message")),A.Pin(Text,TEXT("Message")));
    auto* Equal=A.Fn(UKismetStringLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary,EqualEqual_StrStr)));
    A.Wire(Text ? Text->GetReturnValuePin():nullptr,A.Pin(Equal,TEXT("A")));if (auto* P=A.Pin(Equal,TEXT("B"))) P->DefaultValue=Marker;
    auto* SetMatched=Node<UK2Node_VariableSet>(Graph,A.X,A.Y);SetMatched->VariableReference.SetSelfMember(TEXT("EchoMatched"));SetMatched->AllocateDefaultPins();
    A.Wire(Equal ? Equal->GetReturnValuePin():nullptr,A.Pin(SetMatched,TEXT("EchoMatched")));A.Wire(A.Pin(Message,UEdGraphSchema_K2::PN_Then),A.Pin(SetMatched,UEdGraphSchema_K2::PN_Execute));
    auto* Received=A.SetText(A.Pin(SetMatched,UEdGraphSchema_K2::PN_Then),TEXT("Echo"),A.Prefix(TEXT("Echo: "),Text ? Text->GetReturnValuePin():nullptr));
    auto* Close=ClientCall(GET_FUNCTION_NAME_CHECKED(UVaRestSocketClient,Close));
    if (auto* P=A.Pin(Close,TEXT("Code"))) P->DefaultValue=TEXT("1000");
    if (auto* P=A.Pin(Close,TEXT("Reason"))) P->DefaultValue=TEXT("demo complete");
    A.Wire(Received,Close ? Close->GetExecPin():nullptr);
    A.X=900;A.Y=4200;
    auto* BreakClose=Node<UK2Node_BreakStruct>(Graph,A.X,A.Y+300);BreakClose->StructType=FVaRestSocketClose::StaticStruct();BreakClose->AllocateDefaultPins();
    A.Wire(A.Pin(Closed,TEXT("Close")),A.StructPin(BreakClose,FVaRestSocketClose::StaticStruct(),EGPD_Input));
    auto* Code=A.String(A.Pin(BreakClose,GET_MEMBER_NAME_CHECKED(FVaRestSocketClose,Code)),UKismetStringLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary,Conv_IntToString)),TEXT("InInt"));
    auto* CloseText=A.SetText(A.Pin(Closed,UEdGraphSchema_K2::PN_Then),TEXT("Close"),A.Prefix(TEXT("Local Close: "),Code));
    auto Gate=[&](UEdGraphPin* In,UEdGraphPin* Condition,const TCHAR* Failure) {
        auto* Branch=Node<UK2Node_IfThenElse>(Graph,A.X,A.Y);Branch->AllocateDefaultPins();
        A.Wire(In,Branch->GetExecPin());A.Wire(Condition,Branch->GetConditionPin());
        ErrorTail(A.Pin(Branch,UEdGraphSchema_K2::PN_Else),Failure);A.X+=300;return A.Pin(Branch,UEdGraphSchema_K2::PN_Then);
    };
    auto* Matched=A.Get(TEXT("EchoMatched"));auto* Clean=Gate(CloseText,A.Pin(Matched,TEXT("EchoMatched")),TEXT("Error: echoed text did not match"));
    auto* Is1000=A.Fn(UKismetStringLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary,EqualEqual_StrStr)));
    A.Wire(Code,A.Pin(Is1000,TEXT("A")));if (auto* P=A.Pin(Is1000,TEXT("B"))) P->DefaultValue=TEXT("1000");
    Clean=Gate(Clean,Is1000 ? Is1000->GetReturnValuePin():nullptr,TEXT("Error: unexpected close code"));
    // High-level Close is immediate (WS3.1/D26); final detached wire ACK is not this event.
    // Require its real local notification, never manufacture a clean handshake flag.
    auto IsFalse=[&](FName Member) {
        auto* Not=A.Fn(UKismetMathLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary,Not_PreBool)));
        A.Wire(A.Pin(BreakClose,Member),A.Pin(Not,TEXT("A")));return Not ? Not->GetReturnValuePin():nullptr;
    };
    auto EnumIs=[&](FName Member,UEnum* Enum,int64 Value) -> UEdGraphPin* {
        auto* EqualEnum=Node<UK2Node_EnumEquality>(Graph,A.X,A.Y+300);EqualEnum->AllocateDefaultPins();
        A.Wire(A.Pin(BreakClose,Member),EqualEnum->GetInput1Pin());
        if (!Enum || EqualEnum->GetInput2Pin()->PinType.PinSubCategoryObject.Get()!=Enum || !Enum->IsValidEnumValue(Value))
        { A.Fail(TEXT("Actual close enum pin/type/value unavailable"));return nullptr; }
        EqualEnum->GetInput2Pin()->DefaultValue=Enum->GetNameStringByValue(Value);return EqualEnum->GetReturnValuePin();
    };
    Clean=Gate(Clean,EnumIs(GET_MEMBER_NAME_CHECKED(FVaRestSocketClose,By),StaticEnum<EVaRestSocketCloseBy>(),static_cast<int64>(EVaRestSocketCloseBy::Client)),TEXT("Error: close was not local"));
    Clean=Gate(Clean,EnumIs(GET_MEMBER_NAME_CHECKED(FVaRestSocketClose,Failure),StaticEnum<EVaRestSocketFailure>(),static_cast<int64>(EVaRestSocketFailure::None)),TEXT("Error: local close reported failure"));
    Clean=Gate(Clean,IsFalse(GET_MEMBER_NAME_CHECKED(FVaRestSocketClose,bWasClean)),TEXT("Error: local notification claimed wire ACK"));
    Clean=Gate(Clean,IsFalse(GET_MEMBER_NAME_CHECKED(FVaRestSocketClose,bWillReconnect)),TEXT("Error: local close will reconnect"));
    auto* SameReason=A.Fn(UKismetStringLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary,EqualEqual_StrStr)));
    A.Wire(A.Pin(BreakClose,GET_MEMBER_NAME_CHECKED(FVaRestSocketClose,Reason)),A.Pin(SameReason,TEXT("A")));
    if (auto* P=A.Pin(SameReason,TEXT("B"))) P->DefaultValue=TEXT("demo complete");
    Clean=Gate(Clean,SameReason ? SameReason->GetReturnValuePin():nullptr,TEXT("Error: local close reason differs"));
    // Select camera after actual async callbacks, preserving ordinary controller startup management.
    auto* ResultView=A.Fn(APlayerController::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(APlayerController,SetViewTargetWithBlend)));
    A.Wire(PC ? PC->GetReturnValuePin():nullptr,A.Pin(ResultView,UEdGraphSchema_K2::PN_Self));A.Wire(A.Pin(Self,UEdGraphSchema_K2::PN_Self),A.Pin(ResultView,TEXT("NewViewTarget")));
    A.Wire(Clean,ResultView ? ResultView->GetExecPin():nullptr);
    auto* Done=A.SetText(A.Pin(ResultView,UEdGraphSchema_K2::PN_Then),TEXT("Status"),nullptr,TEXT("Echo matched; local Close requested"));
    A.Wire(Done,QuitBranch->GetExecPin());
    for (auto* Event:{Error,GaveUp})
    {
        A.X=900;A.Y+=1200;
        auto* Break=Node<UK2Node_BreakStruct>(Graph,A.X,A.Y+300);Break->StructType=FVaRestError::StaticStruct();Break->AllocateDefaultPins();
        A.Wire(A.Pin(Event,TEXT("Error")),A.StructPin(Break,FVaRestError::StaticStruct(),EGPD_Input));
        auto* Tail=A.SetText(A.Pin(Event,UEdGraphSchema_K2::PN_Then),TEXT("Status"),A.Prefix(TEXT("Error: "),A.Pin(Break,GET_MEMBER_NAME_CHECKED(FVaRestError,Message))));A.Wire(Tail,QuitBranch->GetExecPin());
    }
    if (!A.Good) return false;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP.Get());FCompilerResultsLog Log;
    FKismetEditorUtilities::CompileBlueprint(BP.Get(),EBlueprintCompileOptions::SkipGarbageCollection,&Log);
    auto* Class=Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
    if (!TestTrue(TEXT("genuine connected public delegate/client graph bytecode"),Log.NumErrors==0 && BP->Status!=BS_Error && Class && Class->UberGraphFunction && !Class->UberGraphFunction->Script.IsEmpty())) return false;
    auto* AutoProperty=FindFProperty<FBoolProperty>(Class,TEXT("AutoCloseAfterDemo"));auto* MatchedProperty=FindFProperty<FBoolProperty>(Class,TEXT("EchoMatched"));
    if (!AutoProperty || !MatchedProperty || AutoProperty->GetPropertyValue_InContainer(Class->GetDefaultObject()) || MatchedProperty->GetPropertyValue_InContainer(Class->GetDefaultObject())) return false;
    FAssetRegistryModule::AssetCreated(BP.Get());Package->MarkPackageDirty();if (!IFileManager::Get().MakeDirectory(*OutputDir,true)) return false;
    FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;Args.SaveFlags=SAVE_NoError;
    if (!UPackage::SavePackage(Package.Get(),BP.Get(),*FPackageName::LongPackageNameToFilename(BlueprintPath,FPackageName::GetAssetPackageExtension()),Args)) return false;
    for (const bool bAuto:{false,true})
    {
        UWorld* World=UEditorLoadingAndSavingUtils::NewBlankMap(false);if (!World) return false;
        FActorSpawnParameters Spawn;Spawn.Name=TEXT("VaRestPublicSocketDemoActor");Spawn.ObjectFlags=RF_Transactional;Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* Actor=World->SpawnActor<AActor>(Class,FVector::ZeroVector,FRotator::ZeroRotator,Spawn);if (!Actor) return false;
        AutoProperty->SetPropertyValue_InContainer(Actor,bAuto);Actor->RerunConstructionScripts();
        if (!TestTrue(TEXT("instance auto-close survives reconstruction"),AutoProperty->GetPropertyValue_InContainer(Actor)==bAuto)) return false;
        auto* Actual=Actor->FindComponentByClass<UVaRestSocketComponent>();TArray<UTextRenderComponent*> Texts;Actor->GetComponents(Texts);
        if (!TestTrue(TEXT("genuine public URL component and seven text rows"),Actual && Actual->Url==TEXT("ws://localhost:18445/echo") && !Actual->bAutoConnect && Actual->Scope==EVaRestSocketScope::Actor && Actual->bCloseOnEndPlay && Texts.Num()==7 && Actor->FindComponentByClass<UCameraComponent>() && Actor->FindComponentByClass<UDirectionalLightComponent>())) return false;
        Actor->SetActorLabel(TEXT("VaRest public WebSocket visual demo"),false);
        if (!UEditorLoadingAndSavingUtils::SaveMap(World,Folder+(bAuto ? TEXT("/FirstLookAuto"):TEXT("/FirstLook")))) return false;
    }
    AddInfo(TEXT("VAREST_SOCKET_SHIPPING_AUTHORED: real public component/client/delegate SendText-echo-Close graph; actual cooked/Shipping wire/visual/native-exit proof remains UNRUN."));
    return true;
}
#endif
