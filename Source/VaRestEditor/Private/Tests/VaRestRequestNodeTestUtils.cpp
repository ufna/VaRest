// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestRequestNodeTestUtils.h"
#include "VaRestRequestNodeExpansion.h"
#include "VaRestRequestAction.h"
#include "VaRestDynamicTransferTestLibrary.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_AssignmentStatement.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_Self.h"
#include "K2Node_TemporaryVariable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace VaRest::Http::Editor::TransferTest
{
namespace
{
template <typename T> T* Node(UEdGraph* Graph) { auto* Result=NewObject<T>(Graph); Graph->AddNode(Result,false,false); Result->CreateNewGuid(); return Result; }
bool Link(UEdGraphPin* A,UEdGraphPin* B) { return A && B && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A,B); }
UK2Node_VariableGet* Get(UEdGraph* Graph,FName Name)
{
	auto* Result=Node<UK2Node_VariableGet>(Graph); Result->VariableReference.SetSelfMember(Name); Result->AllocateDefaultPins(); return Result;
}
UK2Node_VariableSet* Set(UEdGraph* Graph,FName Name)
{
	auto* Result=Node<UK2Node_VariableSet>(Graph); Result->VariableReference.SetSelfMember(Name); Result->AllocateDefaultPins(); return Result;
}
UK2Node_CallFunction* Call(UEdGraph* Graph,UClass* Class,FName Name)
{
	auto* Result=Node<UK2Node_CallFunction>(Graph); Result->SetFromFunction(Class->FindFunctionByName(Name)); Result->AllocateDefaultPins(); return Result;
}
bool Increment(UEdGraph* Graph,UEdGraphPin* Exec,FName Name,UEdGraphPin** OutThen=nullptr)
{
	auto* Add=Call(Graph,UKismetMathLibrary::StaticClass(),GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary,Add_IntInt));
	auto* SetCount=Set(Graph,Name); Add->FindPinChecked(TEXT("B"))->DefaultValue=TEXT("1");
	if (OutThen) *OutThen=SetCount->GetThenPin();
	return Link(Get(Graph,Name)->FindPin(Name),Add->FindPin(TEXT("A"))) && Link(Add->GetReturnValuePin(),SetCount->FindPin(Name)) && Link(Exec,SetCount->GetExecPin());
}
int32 ReadValue(const FProperty* Property,const void* Address)
{
	if (!Property || !Address) return MIN_int32;
	if (const auto* Int=CastField<FIntProperty>(Property)) return Int->GetPropertyValue(Address);
	if (const auto* Struct=CastField<FStructProperty>(Property))
		if (TFieldIterator<FIntProperty> It(Struct->Struct);It) return It->GetPropertyValue_InContainer(Address);
	if (const auto* Array=CastField<FArrayProperty>(Property))
	{
		FScriptArrayHelper Values(Array,Address); if (Values.Num()==1) return ReadValue(Array->Inner,Values.GetRawPtr(0));
	}
	return MIN_int32;
}
bool AddMetadata(UEdGraph* Graph,UEdGraphPin* Action,UEdGraphPin*& Exec,const FString& Identity,int32 Index)
{
	const auto* Property=FindFProperty<FMulticastDelegateProperty>(UVaRestRequestAction::StaticClass(),GET_MEMBER_NAME_CHECKED(UVaRestRequestAction,OnSuccess));
	if (!Property || !Property->SignatureFunction) return false;
	auto* Event=Node<UK2Node_CustomEvent>(Graph); Event->CustomFunctionName=FName(FString::Printf(TEXT("__VaRest_Metadata%d_%s"),Index,*Identity));
	Event->SetDelegateSignature(Property->SignatureFunction); Event->AllocateDefaultPins();
	auto* Bind=Node<UK2Node_AddDelegate>(Graph); Bind->SetFromProperty(Property,false,UVaRestRequestAction::StaticClass()); Bind->AllocateDefaultPins();
	auto* Self=Node<UK2Node_Self>(Graph); Self->AllocateDefaultPins();
	auto* Delegate=Node<UK2Node_CreateDelegate>(Graph); Delegate->AllocateDefaultPins();
	bool bLinked=Link(Exec,Bind->GetExecPin()) && Link(Action,Bind->FindPin(UEdGraphSchema_K2::PN_Self)) &&
		Link(Delegate->GetDelegateOutPin(),Bind->GetDelegatePin()) && Link(Self->FindPin(UEdGraphSchema_K2::PN_Self),Delegate->GetObjectInPin());
	Delegate->SetFunction(Event->GetFunctionName()); Exec=Bind->GetThenPin();
	// A public metadata listener cannot take another typed value after the private preparation has closed.
	auto* Take=Call(Graph,UVaRestRequestAction::StaticClass(),GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,TakeResponse));
	Take->FindPinChecked(TEXT("Value"))->PinType=FEdGraphPinType(UEdGraphSchema_K2::PC_Int,NAME_None,nullptr,EPinContainerType::None,false,FEdGraphTerminalType());
	bLinked=Link(Event->GetThenPin(),Take->GetExecPin()) && Link(Action,Take->FindPin(UEdGraphSchema_K2::PN_Self)) && bLinked;
	auto* Convert=Call(Graph,UKismetMathLibrary::StaticClass(),GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary,Conv_BoolToInt));
	auto* Add=Call(Graph,UKismetMathLibrary::StaticClass(),GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary,Add_IntInt));
	auto* Bad=Set(Graph,TEXT("IllegalTakeCount"));
	bLinked=Link(Take->GetReturnValuePin(),Convert->FindPin(TEXT("InBool"))) && Link(Convert->GetReturnValuePin(),Add->FindPin(TEXT("B"))) &&
		Link(Get(Graph,TEXT("IllegalTakeCount"))->FindPin(TEXT("IllegalTakeCount")),Add->FindPin(TEXT("A"))) &&
		Link(Take->GetThenPin(),Bad->GetExecPin()) && Link(Add->GetReturnValuePin(),Bad->FindPin(TEXT("IllegalTakeCount"))) && bLinked;
	return Increment(Graph,Bad->GetThenPin(),TEXT("MetadataCount")) && bLinked;
}
}
FGraph MakeGraph(const FEdGraphPinType* ResponseType,bool bMultipleMetadata,bool bRetypeContinuation,bool bUserRecompile,bool bObserveError,bool bReentrant)
{
	FGraph Result;
	const FName Name=MakeUniqueObjectName(GetTransientPackage(),UBlueprint::StaticClass(),TEXT("VaRestTransferGate"));
	Result.Blueprint=FKismetEditorUtilities::CreateBlueprint(UObject::StaticClass(),GetTransientPackage(),Name,BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass());
	if (!Result.Blueprint) return Result;
	const FEdGraphPinType Int(UEdGraphSchema_K2::PC_Int,NAME_None,nullptr,EPinContainerType::None,false,FEdGraphTerminalType());
	const FEdGraphPinType String(UEdGraphSchema_K2::PC_String,NAME_None,nullptr,EPinContainerType::None,false,FEdGraphTerminalType());
	const FEdGraphPinType Bool(UEdGraphSchema_K2::PC_Boolean,NAME_None,nullptr,EPinContainerType::None,false,FEdGraphTerminalType());
	const FEdGraphPinType ActionType(UEdGraphSchema_K2::PC_Object,NAME_None,UVaRestRequestAction::StaticClass(),EPinContainerType::None,false,FEdGraphTerminalType());
	for (const FName Field:{FName(TEXT("ContinuationCount")),FName(TEXT("UnavailableCount")),FName(TEXT("MetadataCount")),FName(TEXT("IllegalTakeCount"))})
		if (!FBlueprintEditorUtils::AddMemberVariable(Result.Blueprint,Field,Int)) return Result;
	if (!FBlueprintEditorUtils::AddMemberVariable(Result.Blueprint,TEXT("Observed"),ResponseType ? *ResponseType : Int)) return Result;
	if (!FBlueprintEditorUtils::AddMemberVariable(Result.Blueprint,TEXT("ObservedValid"),Bool) ||
		!FBlueprintEditorUtils::AddMemberVariable(Result.Blueprint,TEXT("ObservedSuccess"),Bool)) return Result;
	if (!FBlueprintEditorUtils::AddMemberVariable(Result.Blueprint,TEXT("ObservedCookie"),String) ||
		!FBlueprintEditorUtils::AddMemberVariable(Result.Blueprint,TEXT("Action"),ActionType)) return Result;
	if (bUserRecompile || bReentrant)
	{
		const FEdGraphPinType ProbeType(UEdGraphSchema_K2::PC_Object,NAME_None,UVaRestDynamicTransferTestProbe::StaticClass(),EPinContainerType::None,false,FEdGraphTerminalType());
		if (!FBlueprintEditorUtils::AddMemberVariable(Result.Blueprint,TEXT("TransferProbe"),ProbeType)) return Result;
	}
	auto* Graph=FBlueprintEditorUtils::CreateNewGraph(Result.Blueprint,TEXT("VaRestTransferEventGraph"),UEdGraph::StaticClass(),UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddUbergraphPage(Result.Blueprint,Graph);
	auto* Start=Node<UK2Node_CustomEvent>(Graph); Start->CustomFunctionName=TEXT("Start"); Start->AllocateDefaultPins(); Result.Start=Start->GetFunctionName();
	auto* Action=Get(Graph,TEXT("Action"))->FindPin(TEXT("Action"));
	const auto* ValueType=ResponseType ? ResponseType : &Int;
	const auto Expansion=ExpandRequestTransfer(Graph,Start,Start->NodeGuid,Action,ValueType,bObserveError ? ValueType : nullptr);
	if (!Expansion.bValid) return Result;
	if (bRetypeContinuation)
	{
		auto* Input=Expansion.Bind->FindPinChecked(TEXT("Continue"));
		const auto Links=Input->LinkedTo;
		for (auto* LinkPin:Links) { auto* Delegate=LinkPin->GetOwningNode(); Delegate->BreakAllNodeLinks(); Graph->RemoveNode(Delegate); }
		const auto* Signature=FindFProperty<FDelegateProperty>(UVaRestRequestAction::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,BindOutput)),TEXT("Prepare"));
		Expansion.Continue->SetDelegateSignature(Signature->SignatureFunction); Expansion.Continue->ReconstructNode();
	}
	Result.Prepare=Expansion.Prepare->GetFunctionName(); Result.Continue=Expansion.Continue->GetFunctionName(); Result.Unavailable=Expansion.Unavailable->GetFunctionName();
	auto* Activate=Call(Graph,UVaRestRequestAction::StaticClass(),GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,Activate));
	auto* Observed=Set(Graph,TEXT("Observed")); auto* Cookie=Set(Graph,TEXT("ObservedCookie"));
	auto* ObservedValid=Set(Graph,TEXT("ObservedValid")); auto* ObservedSuccess=Set(Graph,TEXT("ObservedSuccess"));
	UEdGraphPin* BindingExec=Expansion.BindingThen;
	bool bMetadata=true;
	if (bMultipleMetadata) for (int32 Index=0;Index<2;++Index) bMetadata=AddMetadata(Graph,Action,BindingExec,Start->NodeGuid.ToString(EGuidFormats::Digits),Index) && bMetadata;
	UEdGraphPin* UserThen=nullptr;
	bool bLinked=bMetadata && Link(Start->GetThenPin(),Expansion.Bind->GetExecPin()) && Link(BindingExec,Activate->GetExecPin()) &&
		Link(Action,Activate->FindPin(UEdGraphSchema_K2::PN_Self)) && Link(Expansion.AcceptedExec,Observed->GetExecPin()) &&
		Link((bObserveError ? Expansion.ErrorBody : Expansion.Response)->GetVariablePin(),Observed->FindPin(TEXT("Observed"))) &&
		Link(Observed->GetThenPin(),ObservedValid->GetExecPin()) &&
		Link((bObserveError ? Expansion.ErrorValid : Expansion.ResponseValid)->GetVariablePin(),ObservedValid->FindPin(TEXT("ObservedValid"))) &&
		Link(ObservedValid->GetThenPin(),ObservedSuccess->GetExecPin()) &&
		// The deliberately retyped callback has no bSuccess pin. BindOutput must reject it before any user continuation.
		(bRetypeContinuation || Link(Expansion.Continue->FindPin(TEXT("bSuccess")),ObservedSuccess->FindPin(TEXT("ObservedSuccess")))) &&
		Link(ObservedSuccess->GetThenPin(),Cookie->GetExecPin()) &&
		Link(Expansion.Continue->FindPin(TEXT("RequestIdText")),Cookie->FindPin(TEXT("ObservedCookie"))) &&
		Increment(Graph,Cookie->GetThenPin(),TEXT("ContinuationCount"),&UserThen) && Increment(Graph,Expansion.Unavailable->GetThenPin(),TEXT("UnavailableCount"));
	if (bUserRecompile || bReentrant)
	{
		auto* Compile=Call(Graph,UVaRestDynamicTransferTestLibrary::StaticClass(),bReentrant ? GET_FUNCTION_NAME_CHECKED(UVaRestDynamicTransferTestLibrary,ReenterTarget) :
			GET_FUNCTION_NAME_CHECKED(UVaRestDynamicTransferTestLibrary,RecompileTarget));
		auto* Self=Node<UK2Node_Self>(Graph); Self->AllocateDefaultPins();
		bLinked=Link(UserThen,Compile->GetExecPin()) && Link(Expansion.Continue->FindPin(TEXT("Action")),Compile->FindPin(TEXT("Action"))) &&
			Link(Self->FindPin(UEdGraphSchema_K2::PN_Self),Compile->FindPin(TEXT("Target"))) &&
			Link(Get(Graph,TEXT("TransferProbe"))->FindPin(TEXT("TransferProbe")),Compile->FindPin(TEXT("Probe"))) &&
			Link(Get(Graph,TEXT("Observed"))->FindPin(TEXT("Observed")),Compile->FindPin(TEXT("Value"))) &&
			Link(Get(Graph,TEXT("ObservedCookie"))->FindPin(TEXT("ObservedCookie")),Compile->FindPin(TEXT("Cookie"))) &&
			Link(Get(Graph,TEXT("ContinuationCount"))->FindPin(TEXT("ContinuationCount")),Compile->FindPin(TEXT("ContinuationCount"))) && bLinked;
	}
	// Walk actual generated preparation exec, not a second representation of it.
	TSet<const UK2Node*> Visited;
	const UEdGraphPin* Exec=Expansion.Prepare->GetThenPin(); bool bEndedAtConfirm=false;
	while (bLinked && Exec && Exec->LinkedTo.Num()==1)
	{
		const auto* Next=Cast<UK2Node>(Exec->LinkedTo[0]->GetOwningNode());
		if (!Next || Visited.Contains(Next)) { bLinked=false; break; } Visited.Add(Next);
		if (const auto* CallNode=Cast<UK2Node_CallFunction>(Next))
		{
			const FName Function=CallNode->FunctionReference.GetMemberName();
			if (Function!=GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,TakeResponse) && Function!=GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,TakeErrorBody) &&
				Function!=GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,ConfirmPrepared)) { bLinked=false; break; }
			bEndedAtConfirm=Function==GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,ConfirmPrepared);
		}
		else if (!Next->IsA<UK2Node_AssignmentStatement>()) { bLinked=false; break; }
		Exec=Next->GetThenPin();
	}
	Result.bPreparationIsClosed=bLinked && bEndedAtConfirm && Exec && Exec->LinkedTo.IsEmpty();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Result.Blueprint); return Result;
}
bool Compile(UBlueprint* Blueprint,FCompilerResultsLog& Log)
{
	Log.SetSilentMode(true); FKismetEditorUtilities::CompileBlueprint(Blueprint,EBlueprintCompileOptions::SkipGarbageCollection,&Log);
	return Log.NumErrors==0 && Blueprint->Status!=BS_Error;
}
int32 ReadInt(UObject* Instance,FName Name)
{
	const auto* Property=Instance ? FindFProperty<FIntProperty>(Instance->GetClass(),Name) : nullptr;
	return Property ? Property->GetPropertyValue_InContainer(Instance) : MIN_int32;
}
bool ReadBool(UObject* Instance,FName Name)
{
	const auto* Property=Instance ? FindFProperty<FBoolProperty>(Instance->GetClass(),Name) : nullptr;
	return Property && Property->GetPropertyValue_InContainer(Instance);
}
FString ReadString(UObject* Instance,FName Name)
{
	const auto* Property=Instance ? FindFProperty<FStrProperty>(Instance->GetClass(),Name) : nullptr;
	return Property ? Property->GetPropertyValue_InContainer(Instance) : FString();
}
int32 ReadResponseValue(UObject* Instance)
{
	const auto* Property=Instance ? FindFProperty<FProperty>(Instance->GetClass(),TEXT("Observed")) : nullptr;
	return Property ? ReadValue(Property,Property->ContainerPtrToValuePtr<void>(Instance)) : MIN_int32;
}
bool SetAction(UObject* Instance,UObject* Action)
{
	const auto* Property=Instance ? FindFProperty<FObjectPropertyBase>(Instance->GetClass(),TEXT("Action")) : nullptr;
	if (!Property) return false; Property->SetObjectPropertyValue_InContainer(Instance,Action); return true;
}
bool SetTransferProbe(UObject* Instance,UObject* Probe)
{
	const auto* Property=Instance ? FindFProperty<FObjectPropertyBase>(Instance->GetClass(),TEXT("TransferProbe")) : nullptr;
	if (!Property) return false; Property->SetObjectPropertyValue_InContainer(Instance,Probe); return true;
}
FStorageObservation InspectSharedStorage(UObject* Instance,FStringView Cookie,int32 Value,const UScriptStruct* Struct,bool bError)
{
	FStorageObservation Result;
	const auto* Class=Instance ? Cast<UBlueprintGeneratedClass>(Instance->GetClass()) : nullptr;
	if (!Class || !Class->UberGraphFunction) return Result;
	const auto* Frame=Class->GetPersistentUberGraphFrame(Instance,Class->UberGraphFunction);
	Result.bFrame=Frame!=nullptr; if (!Frame) return Result;
	for (TFieldIterator<FProperty> It(Class->UberGraphFunction); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm)) continue;
		if (const auto* Text=CastField<FStrProperty>(*It); Text && Text->GetName().StartsWith(TEXT("Temp_string_VaRestCookie_")))
			Result.bCookie|=Text->GetPropertyValue_InContainer(Frame)==Cookie;
		if (It->GetName().StartsWith(TEXT("Temp_")) && It->GetName().Contains(bError ? TEXT("_VaRestError_") : TEXT("_VaRestResponse_")))
		{
			const auto* Shape=CastField<FStructProperty>(*It);
			if (const auto* Array=CastField<FArrayProperty>(*It)) Shape=CastField<FStructProperty>(Array->Inner);
			if ((Struct && Shape && Shape->Struct==Struct) || (!Struct && CastField<FIntProperty>(*It)))
				Result.bValue|=ReadValue(*It,It->ContainerPtrToValuePtr<void>(Frame))==Value;
		}
	}
	return Result;
}
bool SupersedeSharedCookie(UObject* Instance,FStringView Cookie)
{
	const auto* Class=Instance ? Cast<UBlueprintGeneratedClass>(Instance->GetClass()) : nullptr;
	if (!Class || !Class->UberGraphFunction) return false;
	auto* Frame=Class->GetPersistentUberGraphFrame(Instance,Class->UberGraphFunction); if (!Frame) return false;
	bool bFound=false;
	for (TFieldIterator<FStrProperty> It(Class->UberGraphFunction); It; ++It)
		if (!It->HasAnyPropertyFlags(CPF_Parm) && It->GetName().StartsWith(TEXT("Temp_string_VaRestCookie_")) && It->GetPropertyValue_InContainer(Frame)==Cookie)
		{ It->SetPropertyValue_InContainer(Frame,TEXT("superseded")); bFound=true; }
	return bFound;
}
}
#endif
