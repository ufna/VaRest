// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
// Explicit real Editor authoring of independent A types, genuine product K2 Blueprint and ordinary map.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestM5EndpointContract.h"
#include "VaRestM5EndpointRunner.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Modules/ModuleManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CoreGlobals.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/UserDefinedEnum.h"
#include "HAL/FileManager.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "VaRestApi.h"
#include "VaRestJsonConvert.h"
#include "VaRestJsonRules.h"

namespace VaRestM5EndpointAuthor
{
bool Dedicated()
{
    return !IsRunningCommandlet() && FParse::Param(FCommandLine::Get(), TEXT("VaRestM5AuthorEndpoint"))
        && FParse::Param(FCommandLine::Get(), TEXT("unattended")) && FParse::Param(FCommandLine::Get(), TEXT("nowrite"));
}
FEdGraphPinType Pin(FName Category, UObject* Type = nullptr, EPinContainerType Container = EPinContainerType::None)
{
    return FEdGraphPinType(Category, NAME_None, Type, Container, false, FEdGraphTerminalType());
}
FGuid FirstBoolean(FAutomationTestBase& Test, UUserDefinedStruct* Struct, const TCHAR* Friendly)
{
    if (!Test.TestTrue(TEXT("new UDS has exactly its real initial boolean"), Struct && FStructureEditorUtils::GetVarDesc(Struct).Num() == 1)) return {};
    const FGuid Guid = FStructureEditorUtils::GetVarDesc(Struct)[0].VarGuid;
    if (!Test.TestTrue(TEXT("rename real initial boolean"), FStructureEditorUtils::RenameVariable(Struct, Guid, Friendly))) return {};
    return Guid;
}
FGuid Add(FAutomationTestBase& Test, UUserDefinedStruct* Struct, const TCHAR* Friendly, const FEdGraphPinType& Type, const TCHAR* Default = nullptr)
{
    if (!Test.TestTrue(FString::Printf(TEXT("add real field %s"), Friendly), FStructureEditorUtils::AddVariable(Struct, Type))) return {};
    const FGuid Guid = FStructureEditorUtils::GetVarDesc(Struct).Last().VarGuid;
    if (!Test.TestTrue(FString::Printf(TEXT("name real field %s"), Friendly), FStructureEditorUtils::RenameVariable(Struct, Guid, Friendly))) return {};
    if (Default && !Test.TestTrue(FString::Printf(TEXT("author default %s"), Friendly), FStructureEditorUtils::ChangeVariableDefaultValue(Struct, Guid, Default))) return {};
    return Guid;
}
void Rule(FVaRestApiJsonTypeRules& Type, FGuid Guid, const TCHAR* Wire, EVaRestJsonPresence Presence = EVaRestJsonPresence::Always,
    bool bNullable = false, bool bBase64 = false)
{
    FVaRestJsonFieldRule Value; Value.Field = Guid; Value.Json = Wire; Value.Presence = Presence;
    Value.bNullable = bNullable; Value.bBase64 = bBase64; Type.Fields.Add(MoveTemp(Value));
}
}


namespace VaRestM5EndpointGraph
{
FVaRestTypeRef ResultType(int32 Case,UObject* Root,UObject* Enum)
{
    FVaRestTypeRef Type;
    if (Case==0 || Case==1 || Case==6) { Type.Kind=EVaRestTypeKind::Struct; Type.Type=Root; }
    if (Case==2) { Type.Kind=EVaRestTypeKind::Int; Type.Container=EVaRestTypeContainer::Array; }
    if (Case==3) { Type.Kind=EVaRestTypeKind::Int; Type.Container=EVaRestTypeContainer::Map; }
    if (Case==4) { Type.Kind=EVaRestTypeKind::Enum; Type.Type=Enum; }
    if (Case==5) Type.Kind=EVaRestTypeKind::DateTime;
    if (Case==7) Type.Kind=EVaRestTypeKind::Int;
    return Type;
}
FEdGraphPinType PinType(const FVaRestTypeRef& Type)
{
    FEdGraphPinType Pin;
    switch (Type.Kind)
    {
    case EVaRestTypeKind::Struct: Pin.PinCategory=UEdGraphSchema_K2::PC_Struct; Pin.PinSubCategoryObject=Type.Type.Get(); break;
    case EVaRestTypeKind::Enum: Pin.PinCategory=UEdGraphSchema_K2::PC_Byte; Pin.PinSubCategoryObject=Type.Type.Get(); break;
    case EVaRestTypeKind::DateTime: Pin.PinCategory=UEdGraphSchema_K2::PC_Struct; Pin.PinSubCategoryObject=TBaseStructure<FDateTime>::Get(); break;
    default: Pin.PinCategory=UEdGraphSchema_K2::PC_Int; break;
    }
    if (Type.Container==EVaRestTypeContainer::Array) Pin.ContainerType=EPinContainerType::Array;
    if (Type.Container==EVaRestTypeContainer::Map)
    {
        Pin.PinCategory=UEdGraphSchema_K2::PC_String; Pin.ContainerType=EPinContainerType::Map;
        Pin.PinValueType.TerminalCategory=UEdGraphSchema_K2::PC_Int;
    }
    return Pin;
}
struct FGraph
{
    FAutomationTestBase& Test; UEdGraph* Graph; bool Good=true;
    template<class T> T* Node()
    {
        auto* N=NewObject<T>(Graph,NAME_None,RF_Transactional);Graph->AddNode(N,false,false);N->CreateNewGuid();return N;
    }
    void Fail(const TCHAR* Detail) { Test.AddError(Detail);Good=false; }
    void Wire(UEdGraphPin* A,UEdGraphPin* B)
    {
        if (!A || !B || !GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A,B)) Fail(TEXT("Actual graph connection refused/missing"));
    }
    void Literal(UEdGraphNode* N,FName Name,const FString& Value)
    {
        auto* Pin=N ? N->FindPin(Name) : nullptr;
        if (!Pin) { Fail(TEXT("Required actual product pin unavailable"));return; }
        Pin->DefaultValue=Value;
    }
    UK2Node_CallFunction* Call(FName Function)
    {
        auto* N=Node<UK2Node_CallFunction>();UFunction* Fn=AVaRestM5EndpointRunner::StaticClass()->FindFunctionByName(Function);
        if (!Fn) { Fail(TEXT("Public fixture function unavailable"));return N; }
        N->SetFromFunction(Fn);N->AllocateDefaultPins();return N;
    }
    UK2Node_VariableGet* Get(FName Name)
    {
        auto* N=Node<UK2Node_VariableGet>();N->VariableReference.SetSelfMember(Name);N->AllocateDefaultPins();return N;
    }
    bool SetStruct(UK2Node* N,FName Name,UScriptStruct* Type,const void* Value)
    {
        auto* P=FindFProperty<FStructProperty>(N->GetClass(),Name);
        if (!P || P->Struct!=Type) { Fail(TEXT("Actual reflected node struct config differs"));return false; }
        P->CopyCompleteValue(P->ContainerPtrToValuePtr<void>(N),Value);return true;
    }
    UK2Node* ProductNode(bool bEndpoint,UVaRestApi* Api,const FVaRestApiEndpoint* Endpoint,const FVaRestTypeRef& Type)
    {
        const TCHAR* Name=bEndpoint ? TEXT("/Script/VaRestEditor.K2Node_VaRestCallEndpoint"):TEXT("/Script/VaRestEditor.K2Node_VaRestSendRequest");
        UClass* Class=FindObject<UClass>(nullptr,Name);
        if (!Class || !Class->IsChildOf(UK2Node::StaticClass()) || !Class->HasAnyClassFlags(CLASS_Native)) { Fail(TEXT("Genuine native product K2 UClass missing"));return nullptr; }
        auto* N=NewObject<UK2Node>(Graph,Class,NAME_None,RF_Transactional);Graph->AddNode(N,false,false);N->CreateNewGuid();
        if (bEndpoint)
        {
            auto* P=FindFProperty<FObjectPropertyBase>(Class,TEXT("Api"));
            auto* Legacy=FindFProperty<FNameProperty>(Class,TEXT("EndpointName"));
            if (!P || !Legacy || !Endpoint) { Fail(TEXT("Actual endpoint reflected config unavailable"));return nullptr; }
            P->SetObjectPropertyValue_InContainer(N,Api);Legacy->SetPropertyValue_InContainer(N,Endpoint->Name);
            SetStruct(N,TEXT("EndpointId"),TBaseStructure<FGuid>::Get(),&Endpoint->Id);
        }
        else SetStruct(N,TEXT("ResponseType"),FVaRestTypeRef::StaticStruct(),&Type);
        if (!Good) return nullptr;
        N->AllocateDefaultPins();return N; // Real product virtual AllocateDefaultPins/ExpandNode, no copied expansion.
    }
    void Reject(UK2Node* Request,UEdGraphPin* Exec,const TCHAR* Reason)
    {
        auto* N=Call(GET_FUNCTION_NAME_CHECKED(AVaRestM5EndpointRunner,Reject));
        N->FindPinChecked(TEXT("Reason"))->DefaultValue=Reason;
        Wire(Exec,N->GetExecPin());Wire(Request->FindPin(TEXT("Http")),N->FindPin(TEXT("Http")));Wire(Request->FindPin(TEXT("Error")),N->FindPin(TEXT("Error")));
    }
};
bool AuthorRunner(FAutomationTestBase& Test,UVaRestApi* Api,UScriptStruct* Root,UEnum* Enum)
{
    using namespace VaRestM5Endpoint;
    FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("VaRestEditor"));
    const FString BPPath=FString(Folder)+TEXT("/SM5_Runner");
    TStrongObjectPtr<UPackage> Package(CreatePackage(*BPPath));
    TStrongObjectPtr<UBlueprint> BP(FKismetEditorUtilities::CreateBlueprint(AVaRestM5EndpointRunner::StaticClass(),Package.Get(),TEXT("SM5_Runner"),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass()));
    if (!BP) { Test.AddError(TEXT("Cannot create ordinary A actor Blueprint"));return false; }
    BP->SetFlags(RF_Public|RF_Standalone|RF_Transactional);
    if (!FBlueprintEditorUtils::AddMemberVariable(BP.Get(),TEXT("Payload"),PinType(ResultType(0,Root,Enum)))) return false;
    for (int32 I=0;I<8;++I)
        if (!FBlueprintEditorUtils::AddMemberVariable(BP.Get(),FName(*FString::Printf(TEXT("Output%d"),I)),PinType(ResultType(I,Root,Enum)))) return false;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP.Get());FCompilerResultsLog Initial;
    FKismetEditorUtilities::CompileBlueprint(BP.Get(),EBlueprintCompileOptions::SkipGarbageCollection,&Initial);
    if (Initial.NumErrors || !BP->GeneratedClass) return false;
    UEdGraph* Graph=FBlueprintEditorUtils::FindEventGraph(BP.Get());if (!Graph) return false;
    FGraph G{Test,Graph};
    TArray<UK2Node_Event*> Events;FBlueprintEditorUtils::GetAllNodesOfClass(BP.Get(),Events);UK2Node_Event* Begin=nullptr;
    for (auto* E:Events) if (E->EventReference.GetMemberName()==TEXT("ReceiveBeginPlay")) { if (Begin) return false;Begin=E; }
    if (!Begin) { Begin=G.Node<UK2Node_Event>();Begin->EventReference.SetExternalMember(TEXT("ReceiveBeginPlay"),AActor::StaticClass());Begin->bOverrideFunction=true;Begin->AllocateDefaultPins(); }
    auto* Arm=G.Call(GET_FUNCTION_NAME_CHECKED(AVaRestM5EndpointRunner,Arm));
    auto* Armed=G.Node<UK2Node_IfThenElse>();Armed->AllocateDefaultPins();
    G.Wire(Begin->FindPin(UEdGraphSchema_K2::PN_Then),Arm->GetExecPin());G.Wire(Arm->GetThenPin(),Armed->GetExecPin());G.Wire(Arm->GetReturnValuePin(),Armed->GetConditionPin());
    UEdGraphPin* Previous=Armed->GetThenPin();
    for (int32 I=0;I<8;++I)
    {
        const bool bEndpoint=(I%2)==0;const FVaRestApiEndpoint* Endpoint=bEndpoint ? Api->FindEndpoint(FName(*FString::Printf(TEXT("Case%d"),I))):nullptr;
        auto* Request=G.ProductNode(bEndpoint,Api,Endpoint,ResultType(I,Root,Enum));if (!Request) return false;
        Request->NodePosX=I*900;Request->NodePosY=100;
        G.Wire(Previous,Request->FindPin(UEdGraphSchema_K2::PN_Execute));
        auto* Payload=G.Get(TEXT("Payload"));G.Wire(Payload->FindPin(TEXT("Payload")),Request->FindPin(TEXT("Body")));
        if (!bEndpoint)
        {
            G.Literal(Request,TEXT("Verb"),TEXT("POST"));
            G.Literal(Request,TEXT("Path"),FString::Printf(TEXT("/m5/send/%d"),I));
            G.Literal(Request,TEXT("AuthMode"),StaticEnum<EVaRestAuthMode>()->GetNameStringByValue(static_cast<int64>(EVaRestAuthMode::None)));
            auto* Client=G.Get(TEXT("Client"));G.Wire(Client->FindPin(TEXT("Client")),Request->FindPin(TEXT("Client")));
        }
        auto* Set=G.Node<UK2Node_VariableSet>();const FName Name(*FString::Printf(TEXT("Output%d"),I));Set->VariableReference.SetSelfMember(Name);Set->AllocateDefaultPins();
        G.Wire(Request->FindPin(I==6 ? TEXT("OnError"):TEXT("OnSuccess")),Set->FindPin(UEdGraphSchema_K2::PN_Execute));
        G.Wire(Request->FindPin(I==6 ? TEXT("ErrorBody"):TEXT("Response")),Set->FindPin(Name));
        auto* Check=G.Call(GET_FUNCTION_NAME_CHECKED(AVaRestM5EndpointRunner,CheckCase));
        Check->FindPinChecked(TEXT("Case"))->DefaultValue=FString::FromInt(I);
        G.Wire(Set->FindPin(UEdGraphSchema_K2::PN_Then),Check->GetExecPin());G.Wire(Request->FindPin(TEXT("Http")),Check->FindPin(TEXT("Http")));G.Wire(Request->FindPin(TEXT("Error")),Check->FindPin(TEXT("Error")));
        if (I==6) G.Wire(Request->FindPin(TEXT("ErrorBodyValid")),Check->FindPin(TEXT("bErrorBodyValid")));
        auto* Accepted=G.Node<UK2Node_IfThenElse>();Accepted->AllocateDefaultPins();G.Wire(Check->GetThenPin(),Accepted->GetExecPin());G.Wire(Check->GetReturnValuePin(),Accepted->GetConditionPin());Previous=Accepted->GetThenPin();
        G.Reject(Request,Request->FindPin(I==6 ? TEXT("OnSuccess"):TEXT("OnError")),TEXT("Unexpected primary outcome"));
        G.Reject(Request,Request->FindPin(TEXT("OnTypedOutputUnavailable")),TEXT("Actual typed output unavailable"));
    }
    if (!G.Good) return false;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP.Get());FCompilerResultsLog Log;
    FKismetEditorUtilities::CompileBlueprint(BP.Get(),EBlueprintCompileOptions::SkipGarbageCollection,&Log);
    auto* Class=Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
    if (Log.NumErrors || BP->Status==BS_Error || !Class || !Class->UberGraphFunction || Class->UberGraphFunction->Script.IsEmpty()) return false;
    auto* Defaults=Cast<AVaRestM5EndpointRunner>(Class->GetDefaultObject());
    auto* Payload=FindFProperty<FStructProperty>(Class,TEXT("Payload"));FString Error;
    if (!Defaults || !Payload || Payload->Struct!=Root) return false;
    Defaults->Api=Api;
    if (!PreparePayload(Api,Root,Payload->ContainerPtrToValuePtr<void>(Defaults),Error)) { Test.AddError(Error);return false; }
    Class->UpdateCustomPropertyListForPostConstruction();
    FAssetRegistryModule::AssetCreated(BP.Get());Package->MarkPackageDirty();
    FSavePackageArgs Save;Save.TopLevelFlags=RF_Public|RF_Standalone;Save.SaveFlags=SAVE_NoError;
    if (!UPackage::SavePackage(Package.Get(),BP.Get(),*FPackageName::LongPackageNameToFilename(BPPath,FPackageName::GetAssetPackageExtension()),Save)) return false;
    UWorld* World=UEditorLoadingAndSavingUtils::NewBlankMap(false);if (!World) return false;
    FActorSpawnParameters Spawn;Spawn.Name=TEXT("SM5_Runner");Spawn.ObjectFlags=RF_Transactional;Spawn.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AActor* Actor=World->SpawnActor<AActor>(Class,FVector::ZeroVector,FRotator::ZeroRotator,Spawn);if (!Actor) return false;
    const auto* Inherited=Cast<AVaRestM5EndpointRunner>(Actor);
    const auto* InheritedPayload=FindFProperty<FStructProperty>(Actor->GetClass(),TEXT("Payload"));
    if (!Test.TestTrue(TEXT("fresh actor inherits exact API and default-encoded payload"),
        Inherited && Inherited->Api==Api && Inherited->Api.GetPathName().Equals(VaRestM5Endpoint::ApiPath,ESearchCase::CaseSensitive)
        && InheritedPayload && InheritedPayload->Struct==Root
        && FVaRestJsonConvert::StructToString(InheritedPayload->Struct,InheritedPayload->ContainerPtrToValuePtr<void>(Inherited)).Equals(VaRestM5Endpoint::WireBody(),ESearchCase::CaseSensitive))) return false;
    Test.AddInfo(TEXT("VAREST_M5_ENDPOINT_SPAWN_DEFAULTS_VERIFIED exactApi=1 defaultEncodedPayload=1 noInstanceRepair=1"));
    Actor->SetActorLabel(TEXT("VaRest M5 real endpoint/send fixture"),false);
    if (!UEditorLoadingAndSavingUtils::SaveMap(World,FString(Folder)+TEXT("/SM5_Map"))) return false;
    Test.AddInfo(TEXT("VAREST_M5_ENDPOINT_RUNNER_AUTHORED genuineNativeK2=8 endpoint=4 send=4 bytecode=1 map=SM5_Map runtimeCookWire=UNRUN"));
    return true;
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestM5EndpointAuthor, "VaRest.M5.Endpoint.Author",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FVaRestM5EndpointAuthor::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    if (VaRestM5EndpointAuthor::Dedicated()) { Names.Add(TEXT("Endpoint001")); Commands.Add(TEXT("Endpoint001")); }
}
bool FVaRestM5EndpointAuthor::RunTest(const FString& Parameters)
{
    using namespace VaRestM5Endpoint;
    using namespace VaRestM5EndpointAuthor;
    if (!Dedicated() || Parameters != TEXT("Endpoint001") || !GEditor || !GEditor->Trans)
    { AddError(TEXT("Requires the dedicated unattended nowrite real Editor process with transaction buffer")); return false; }
    const TArray<FString> Packages {
        TEXT("/VaRestM5Smoke/Endpoint001/Types/SM5_State"),
        TEXT("/VaRestM5Smoke/Endpoint001/Types/SM5_Nested"),
        TEXT("/VaRestM5Smoke/Endpoint001/Types/SM5_Response"),
        TEXT("/VaRestM5Smoke/Endpoint001/SM5_Api"),
        TEXT("/VaRestM5Smoke/Endpoint001/SM5_Runner"),
        TEXT("/VaRestM5Smoke/Endpoint001/SM5_Map")
    };
    FString OutputFolder;
    if (!FPackageName::TryConvertLongPackageNameToFilename(FString(Folder), OutputFolder)
        || IFileManager::Get().DirectoryExists(*OutputFolder))
    { AddError(TEXT("Fixture content mount unavailable or fresh owned namespace already occupied; no overwrite")); return false; }
    for (const FString& Package : Packages)
        if (FindPackage(nullptr, *Package) || FPackageName::DoesPackageExist(Package))
        { AddError(TEXT("An owned fixture package is already loaded/on disk; no overwrite")); return false; }

    TStrongObjectPtr<UPackage> EnumPackage(CreatePackage(*Packages[0]));
    TStrongObjectPtr<UPackage> NestedPackage(CreatePackage(*Packages[1]));
    TStrongObjectPtr<UPackage> RootPackage(CreatePackage(*Packages[2]));
    TStrongObjectPtr<UPackage> ApiPackage(CreatePackage(*Packages[3]));
    constexpr EObjectFlags AssetFlags = RF_Public | RF_Standalone | RF_Transactional;
    TStrongObjectPtr<UUserDefinedEnum> State(Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(
        EnumPackage.Get(), TEXT("SM5_State"), AssetFlags)));
    TStrongObjectPtr<UUserDefinedStruct> Nested(FStructureEditorUtils::CreateUserDefinedStruct(
        NestedPackage.Get(), TEXT("SM5_Nested"), AssetFlags));
    TStrongObjectPtr<UUserDefinedStruct> Root(FStructureEditorUtils::CreateUserDefinedStruct(
        RootPackage.Get(), TEXT("SM5_Response"), AssetFlags));
    TStrongObjectPtr<UVaRestApi> Api(NewObject<UVaRestApi>(ApiPackage.Get(), TEXT("SM5_Api"), AssetFlags));
    if (!TestTrue(TEXT("four genuine independent asset objects"), State && Nested && Root && Api)) return false;
    for (int32 Index = 0; Index != 3; ++Index) FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(State.Get());
    if (!TestEqual(TEXT("three real enum entries plus MAX"), State->NumEnums(), 4)
        || !TestTrue(TEXT("real pretty unset display name"), FEnumEditorUtils::SetEnumeratorDisplayName(State.Get(), 0, FText::FromString(TEXT("Unset Display"))))
        || !TestTrue(TEXT("real mapped display differs from wire"), FEnumEditorUtils::SetEnumeratorDisplayName(State.Get(), 1, FText::FromString(TEXT("Pretty State One"))))
        || !TestTrue(TEXT("real unmapped pretty display name"), FEnumEditorUtils::SetEnumeratorDisplayName(State.Get(), 2, FText::FromString(TEXT("Pretty State Two"))))) return false;

    FVaRestApiJsonTypeRules NestedRules; NestedRules.Type = Nested.Get();
    Rule(NestedRules, FirstBoolean(*this, Nested.Get(), TEXT("Nested Flag")), TEXT("Nested.Flag"));
    Rule(NestedRules, Add(*this, Nested.Get(), TEXT("Nested Value"), Pin(UEdGraphSchema_K2::PC_Int)), TEXT("Nested.Value"));
    FVaRestApiJsonTypeRules RootRules; RootRules.Type = Root.Get();
    Rule(RootRules, FirstBoolean(*this, Root.Get(), TEXT("Always Flag")), TEXT("ExactFlag"));
    const FGuid RenamedGuid = Add(*this, Root.Get(), TEXT("Original Score"), Pin(UEdGraphSchema_K2::PC_Int), TEXT("42"));
    Rule(RootRules, RenamedGuid, TEXT("Wire.Score"));
    const FGuid SanitizedGuid = Add(*this, Root.Get(), TEXT("Sanitized Label"), Pin(UEdGraphSchema_K2::PC_String));
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Omit Count"), Pin(UEdGraphSchema_K2::PC_Int), TEXT("42")), TEXT("omitCount"), EVaRestJsonPresence::OmitWhenDefault);
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Null Count"), Pin(UEdGraphSchema_K2::PC_Int), TEXT("42")), TEXT("nullCount"), EVaRestJsonPresence::NullWhenDefault);
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Decode Only"), Pin(UEdGraphSchema_K2::PC_String)), TEXT("decodeOnly"), EVaRestJsonPresence::DecodeOnly);
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Ignored Text"), Pin(UEdGraphSchema_K2::PC_String), TEXT("kept")), TEXT("ignored"), EVaRestJsonPresence::Ignore);
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Nullable Text"), Pin(UEdGraphSchema_K2::PC_String), TEXT("kept")), TEXT("nullable"), EVaRestJsonPresence::Always, true);
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Blob Bytes"), Pin(UEdGraphSchema_K2::PC_Byte, nullptr, EPinContainerType::Array)), TEXT("bytes"), EVaRestJsonPresence::Always, false, true);
    const FEdGraphPinType EnumScalar = Pin(UEdGraphSchema_K2::PC_Byte, State.Get());
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Unset State"), EnumScalar), TEXT("state"));
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Null State"), EnumScalar), TEXT("nullState"), EVaRestJsonPresence::NullWhenDefault);
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Mapped State"), EnumScalar), TEXT("mapped"));
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Pretty State"), EnumScalar), TEXT("pretty"));
    Rule(RootRules, Add(*this, Root.Get(), TEXT("States"), Pin(UEdGraphSchema_K2::PC_Byte, State.Get(), EPinContainerType::Array)), TEXT("states"));
    FEdGraphPinType EnumMap = Pin(UEdGraphSchema_K2::PC_String, nullptr, EPinContainerType::Map);
    EnumMap.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Byte;
    EnumMap.PinValueType.TerminalSubCategoryObject = State.Get();
    Rule(RootRules, Add(*this, Root.Get(), TEXT("State Map"), EnumMap), TEXT("stateMap"));
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Nested"), Pin(UEdGraphSchema_K2::PC_Struct, Nested.Get())), TEXT("nested"));
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Scalars"), Pin(UEdGraphSchema_K2::PC_Int, nullptr, EPinContainerType::Array)), TEXT("scalars"));
    FEdGraphPinType ScalarMap = Pin(UEdGraphSchema_K2::PC_String, nullptr, EPinContainerType::Map);
    ScalarMap.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Int;
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Scalar Map"), ScalarMap), TEXT("scalarMap"));
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Stamp"), Pin(UEdGraphSchema_K2::PC_Struct, TBaseStructure<FDateTime>::Get())), TEXT("stamp"));
    if (!RenamedGuid.IsValid() || !SanitizedGuid.IsValid()) return false;
    for (const auto* Type : { &NestedRules, &RootRules })
        for (const FVaRestJsonFieldRule& Field : Type->Fields)
            if (!TestTrue(TEXT("every authored rule has an actual field GUID"), Field.Field.IsValid())) return false;

    FVaRestApiJsonTypeRules EnumRules; EnumRules.Type = State.Get();
    FVaRestJsonEnumRule Unset; Unset.Enumerator = State->GetNameByIndex(0); Unset.bUnset = true;
    FVaRestJsonEnumRule One; One.Enumerator = State->GetNameByIndex(1); One.Json = TEXT("wire-ONE");
    EnumRules.Enums = { Unset, One }; // Index 2 intentionally uses its real prettified authored display text.
    Api->JsonTypes = { RootRules, NestedRules, EnumRules };
    Api->TypesFolder = FString(Folder) + TEXT("/Types");
    FVaRestApiEnvironment Environment; Environment.Name = TEXT("Endpoint"); int32 Port=18765; FParse::Value(FCommandLine::Get(),TEXT("VaRestM5ServerPort="),Port);
    if (Port<1024 || Port>65535) { AddError(TEXT("Invalid owned loopback server port"));return false; }
    Environment.BaseUrl = FString::Printf(TEXT("http://127.0.0.1:%d"),Port);
    Api->Environments.Add(Environment); Api->DefaultEnvironment = Environment.Name;
    Api->Retry=FVaRestRetryPolicy::Disabled(); Api->bStrictDecode=true; Api->Timeout=5.f; Api->ActivityTimeout=5.f;
    FVaRestHeader First;First.Name=TEXT("X-M5-Dup");First.Value=TEXT("first");
    FVaRestHeader Second;Second.Name=TEXT("X-M5-Dup");Second.Value=TEXT("second");Api->Headers={First,Second};
    for (int32 I:{0,2,4,6})
    {
        FVaRestApiEndpoint E;E.Name=FName(*FString::Printf(TEXT("Case%d"),I));E.Verb=TEXT("POST");E.Path=FString::Printf(TEXT("/m5/endpoint/%d"),I);
        E.AuthMode=EVaRestAuthMode::None;E.RequestType=VaRestM5EndpointGraph::ResultType(0,Root.Get(),State.Get());E.ResponseType=VaRestM5EndpointGraph::ResultType(I,Root.Get(),State.Get());
        if (I==6) E.ErrorType=VaRestM5EndpointGraph::ResultType(0,Root.Get(),State.Get());
        if (I==0)
        {
            FVaRestEndpointParam Lower;Lower.Name=TEXT("sort");Lower.PinName=TEXT("sortLower");Lower.In=EVaRestEndpointParamLocation::Query;Lower.Type.Kind=EVaRestTypeKind::String;Lower.bRequired=true;Lower.Default=TEXT("lower");
            FVaRestEndpointParam Upper=Lower;Upper.Name=TEXT("Sort");Upper.PinName=TEXT("sortUpper");Upper.Default=TEXT("UPPER");E.Params={Lower,Upper};
        }
        Api->Endpoints.Add(E);
    }
    Api->NotifyEdited();
    FString Error;
    if (!TestTrue(TEXT("real editor default conversion before GUID-preserving rename"), VerifyDefaultConversion(Api.Get(), Root.Get(), Error)))
    { AddError(Error); return false; }
    const uint64 RulesGeneration = FVaRestJsonRules::GetGeneration(Api.Get());
    if (!TestTrue(TEXT("actual editor rename/recompile retains original field GUID"), FStructureEditorUtils::RenameVariable(Root.Get(), RenamedGuid, TEXT("Renamed Score")))
        || !TestEqual(TEXT("rename needs no rule republish"), FVaRestJsonRules::GetGeneration(Api.Get()), RulesGeneration)) return false;
    if (!TestTrue(TEXT("actual renamed cooked property name"), Root->FindPropertyNameFromGuid(RenamedGuid).ToString().StartsWith(TEXT("RenamedScore_"), ESearchCase::CaseSensitive))) return false;
    if (!VerifyDefaultConversion(Api.Get(), Root.Get(), Error)) { AddError(Error); return false; }
    FVaRestApiValidationContext Validation; Api->Validate(Validation);
    if (!TestFalse(TEXT("actual API validation has no configuration/rule errors"), Validation.HasErrors())) return false;

    const TArray<UObject*> Assets { State.Get(), Nested.Get(), Root.Get(), Api.Get() };
    if (!IFileManager::Get().MakeDirectory(*OutputFolder, true)) { AddError(TEXT("Cannot create fresh owned output namespace")); return false; }
    for (int32 Index = 0; Index != Assets.Num(); ++Index)
    {
        UObject* Asset = Assets[Index]; UPackage* Package = Asset->GetOutermost();
        FAssetRegistryModule::AssetCreated(Asset); Package->MarkPackageDirty();
        const FString Filename = FPackageName::LongPackageNameToFilename(Packages[Index], FPackageName::GetAssetPackageExtension());
        if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true)) { AddError(TEXT("Cannot create owned type directory")); return false; }
        FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
        if (!UPackage::SavePackage(Package, Asset, *Filename, Save)) { AddError(TEXT("Owned fixture save failed; retain partial output as failed evidence, never overwrite it")); return false; }
        AddInfo(FString::Printf(TEXT("VAREST_M5_ENDPOINT_SAVED package=%s object=%s filename=%s"), *Packages[Index], *Asset->GetPathName(), *Filename));
    }
    if (!VaRestM5EndpointGraph::AuthorRunner(*this,Api.Get(),Root.Get(),State.Get())) return false;
    AddInfo(FString::Printf(TEXT("VAREST_M5_ENDPOINT_AUTHORED proof=A namespace=%s renamedFieldGuid=%s sanitizedFieldGuid=%s editorDefaultConversion=PASS packages=6 runtimeCookWireMutants=UNRUN wholeTask14=OPEN"),
        Folder, *RenamedGuid.ToString(EGuidFormats::Digits), *SanitizedGuid.ToString(EGuidFormats::Digits)));
    return true;
}
#endif
