// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestK2RequestExpansion.h"
#include "K2Node_VaRestSendRequest.h"
#include "K2Node_VaRestCallEndpoint.h"
#include "VaRestApi.h"
#include "VaRestRequestAction.h"
#include "VaRestRequestHandle.h"
#include "VaRestRequestNodeExpansion.h"
#include "VaRestTypeRefDetails.h"
#include "VaRestArrayParamDefaults.h"
#include "VaRestJson.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_AssignmentStatement.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_MakeArray.h"
#include "K2Node_Self.h"
#include "K2Node_TemporaryVariable.h"
#include "Kismet/KismetSystemLibrary.h"
#include "KismetCompiler.h"
#include "UObject/NoExportTypes.h"
#include "UObject/UnrealType.h"

namespace VaRest::Http::Editor
{
namespace FullRequestPrivate
{
struct FBuilder
{
	UK2Node_VaRestRequestBase& Source;
	FKismetCompilerContext& Compiler;
	UEdGraph* Graph;
	const UEdGraphSchema_K2* Schema=GetDefault<UEdGraphSchema_K2>();
	uint32 Role=0;
	bool bValid=true;
	template <typename T> T* Node()
	{
		auto* Result=Compiler.SpawnIntermediateNode<T>(&Source,Graph);
		Result->NodeGuid=FGuid::Combine(Source.NodeGuid,FGuid(0x5652324b,0,0,++Role)); return Result;
	}
	void Link(UEdGraphPin* A,UEdGraphPin* B) { bValid=bool(A && B && Schema->TryCreateConnection(A,B)) && bValid; }
	void MoveInput(UEdGraphPin* Input,UEdGraphPin* Destination)
	{
		if (!Input || !Destination) { bValid=false;return; }
		const bool bLinked=!Input->LinkedTo.IsEmpty();
		Compiler.MovePinLinksToIntermediate(*Input,*Destination);
		if (!bLinked) { Destination->DefaultValue=Input->DefaultValue; Destination->DefaultObject=Input->DefaultObject; Destination->DefaultTextValue=Input->DefaultTextValue; }
	}
	void Output(FName Name,UEdGraphPin* Pin)
	{
		if (auto* Original=Source.FindPin(Name)) { if (Pin) Compiler.MovePinLinksToIntermediate(*Original,*Pin); else bValid=false; }
	}
	UK2Node_CallFunction* Call(UClass* Class,FName Function)
	{
		auto* Result=Node<UK2Node_CallFunction>(); Result->SetFromFunction(Class->FindFunctionByName(Function)); Result->AllocateDefaultPins(); return Result;
	}
	UK2Node_CallFunction* ActionCall(FName Function,UEdGraphPin* Action)
	{
		auto* Result=Call(UVaRestRequestAction::StaticClass(),Function);Link(Action,Result->FindPin(UEdGraphSchema_K2::PN_Self));return Result;
	}
	UK2Node_TemporaryVariable* Variable(const FEdGraphPinType& Type,const TCHAR* Name)
	{
		auto* Result=Node<UK2Node_TemporaryVariable>();Result->VariableType=Type;Result->bIsPersistent=false;
		Result->NodeComment=FString(TEXT("VaRestFull_"))+Name+Source.NodeGuid.ToString(EGuidFormats::Digits);Result->AllocateDefaultPins();return Result;
	}
	void Assign(UEdGraphPin*& Exec,UK2Node_TemporaryVariable* Variable,UEdGraphPin* Value,const TCHAR* Literal=nullptr)
	{
		if (!Variable) return;
		auto* Set=Node<UK2Node_AssignmentStatement>();Set->AllocateDefaultPins(); Link(Exec,Set->GetExecPin());Link(Variable->GetVariablePin(),Set->GetVariablePin());Set->NotifyPinConnectionListChanged(Set->GetVariablePin());
		if (Value) Link(Value,Set->GetValuePin());else if (Literal) Set->GetValuePin()->DefaultValue=Literal;
		Set->NotifyPinConnectionListChanged(Set->GetValuePin());Exec=Set->GetThenPin();
	}
	UEdGraphPin* Struct(UScriptStruct* Type,UK2Node_MakeStruct*& Out)
	{
		Out=Node<UK2Node_MakeStruct>();Out->StructType=Type;Out->bMadeAfterOverridePinRemoval=true;Out->AllocateDefaultPins();
		for (auto* Pin:Out->Pins) if (Pin->Direction==EGPD_Output && Pin->PinType.PinCategory==UEdGraphSchema_K2::PC_Struct) return Pin;
		bValid=false;return nullptr;
	}
	UEdGraphPin* TypeLiteral(const FVaRestTypeRef& Type)
	{
		UK2Node_MakeStruct* Make=nullptr;auto* Output=Struct(FVaRestTypeRef::StaticStruct(),Make);
		Make->FindPinChecked(TEXT("Kind"))->DefaultValue=StaticEnum<EVaRestTypeKind>()->GetNameStringByValue(static_cast<int64>(Type.Kind));
		Make->FindPinChecked(TEXT("Container"))->DefaultValue=StaticEnum<EVaRestTypeContainer>()->GetNameStringByValue(static_cast<int64>(Type.Container));
		Make->FindPinChecked(TEXT("bJsonValue"))->DefaultValue=Type.bJsonValue ? TEXT("true") : TEXT("false");
		Make->FindPinChecked(TEXT("Type"))->DefaultObject=Type.Type.Get();return Output;
	}
	UEdGraphPin* Getter(FName Function,UEdGraphPin* Action) { return ActionCall(Function,Action)->GetReturnValuePin(); }
	UK2Node_IfThenElse* Branch(UEdGraphPin* Exec,UEdGraphPin* Condition)
	{
		auto* Result=Node<UK2Node_IfThenElse>();Result->AllocateDefaultPins();Link(Exec,Result->GetExecPin());Link(Condition,Result->GetConditionPin());return Result;
	}
};
FEdGraphPinType PinType(FName Category,UObject* Object=nullptr)
{
	return FEdGraphPinType(Category,NAME_None,Object,EPinContainerType::None,false,FEdGraphTerminalType());
}
FName RawGetter(const FVaRestTypeRef& Type,bool bError)
{
	if (Type.Kind==EVaRestTypeKind::Json) return bError ? GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,GetErrorJson) : GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,GetJson);
	if (Type.Kind==EVaRestTypeKind::String) return bError ? GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,GetErrorText) : GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,GetText);
	return bError ? GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,GetErrorBytes) : GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,GetBytes);
}
}
void ExpandFullRequest(UK2Node_VaRestRequestBase& Source,FKismetCompilerContext& Compiler,UEdGraph* Graph)
{
	using namespace FullRequestPrivate;
	FBuilder B{Source,Compiler,Graph};
	const FVaRestTypeRef ResponseType=Source.GetResponseType(),ErrorType=Source.GetErrorType();
	const bool bTypedResponse=IsTransferredType(ResponseType),bTypedError=IsTransferredType(ErrorType);
	FEdGraphPinType ResponsePin,ErrorPin;FString TypeError;
	if (ResponseType.Kind!=EVaRestTypeKind::None && !TypeToPin(ResponseType,ResponsePin,TypeError)) { Compiler.MessageLog.Error(TEXT("@@ response type is invalid."),&Source);return; }
	if (ErrorType.Kind!=EVaRestTypeKind::None && !TypeToPin(ErrorType,ErrorPin,TypeError)) { Compiler.MessageLog.Error(TEXT("@@ error type is invalid."),&Source);return; }
	auto* Self=B.Node<UK2Node_Self>();Self->AllocateDefaultPins();auto* Context=Self->FindPin(UEdGraphSchema_K2::PN_Self);
	const auto* Endpoint=Source.ResolveEndpoint();
	auto* Factory=B.Call(UVaRestRequestAction::StaticClass(),Source.IsEndpointNode() ? GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,CreateEndpointRequest) : GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,CreateRequest));
	B.Link(Context,Factory->FindPin(TEXT("WorldContextObject")));
	if (Source.IsEndpointNode())
	{
		const auto* Call=CastChecked<UK2Node_VaRestCallEndpoint>(&Source);
		Factory->FindPinChecked(TEXT("Api"))->DefaultObject=Call->Api.Get();
		FGuid Id=Call->EndpointId.IsValid() ? Call->EndpointId : Endpoint->Id;FString Text;
		TBaseStructure<FGuid>::Get()->ExportText(Text,&Id,nullptr,nullptr,PPF_None,nullptr);Factory->FindPinChecked(TEXT("EndpointId"))->DefaultValue=Text;
		Factory->FindPinChecked(TEXT("EndpointName"))->DefaultValue=Endpoint->Name.ToString();
	}
	else
	{
		for (FName Name:{FName(TEXT("Client")),FName(TEXT("Verb")),FName(TEXT("Path"))}) B.MoveInput(Source.FindPin(Name),Factory->FindPin(Name));
		B.Link(B.TypeLiteral(ResponseType),Factory->FindPin(TEXT("ResponseType")));B.Link(B.TypeLiteral(ErrorType),Factory->FindPin(TEXT("ErrorType")));
	}
	auto* Tag=B.Variable(PinType(UEdGraphSchema_K2::PC_Name),TEXT("Tag"));
	auto* CaptureTag=B.Node<UK2Node_AssignmentStatement>();CaptureTag->AllocateDefaultPins();B.Link(Tag->GetVariablePin(),CaptureTag->GetVariablePin());CaptureTag->NotifyPinConnectionListChanged(CaptureTag->GetVariablePin());
	B.MoveInput(Source.FindPin(TEXT("Tag")),CaptureTag->GetValuePin());CaptureTag->NotifyPinConnectionListChanged(CaptureTag->GetValuePin());
	Compiler.MovePinLinksToIntermediate(*Source.GetExecPin(),*CaptureTag->GetExecPin());B.Link(CaptureTag->GetThenPin(),Factory->GetExecPin());
	if (Source.IsEndpointNode()) B.Link(Tag->GetVariablePin(),Factory->FindPin(TEXT("Tag")));
	auto* Action=B.Variable(PinType(UEdGraphSchema_K2::PC_Object,UVaRestRequestAction::StaticClass()),TEXT("Action"));
	UEdGraphPin* Exec=Factory->GetThenPin();B.Assign(Exec,Action,Factory->GetReturnValuePin());auto* CurrentAction=Action->GetVariablePin();
	auto* IsValid=B.Call(UKismetSystemLibrary::StaticClass(),GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary,IsValid));B.Link(CurrentAction,IsValid->FindPin(TEXT("Object")));
	auto* Admitted=B.Branch(Exec,IsValid->GetReturnValuePin());Exec=Admitted->GetThenPin();
	auto* Configure=B.ActionCall(GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,ConfigureRequest),CurrentAction);B.Link(Exec,Configure->GetExecPin());Exec=Configure->GetThenPin();
	B.MoveInput(Source.FindPin(TEXT("Query")),Configure->FindPin(TEXT("Query")));B.Link(Tag->GetVariablePin(),Configure->FindPin(TEXT("Tag")));
	Configure->FindPinChecked(TEXT("bQueueWhenOffline"))->DefaultValue=Source.AllowsQueue() ? TEXT("true") : TEXT("false");
	if (Endpoint) Configure->FindPinChecked(TEXT("AuthMode"))->DefaultValue=StaticEnum<EVaRestAuthMode>()->GetNameStringByValue(static_cast<int64>(Endpoint->AuthMode));
	else
	{
		B.MoveInput(Source.FindPin(TEXT("Headers")),Configure->FindPin(TEXT("Headers")));B.MoveInput(Source.FindPin(TEXT("AuthMode")),Configure->FindPin(TEXT("AuthMode")));
	}
	const auto Parameter=[&](UEdGraphPin* Input,const FString& Name,EVaRestEndpointParamLocation Location,EVaRestQueryStyle Style,const TArray<FString>* Defaults=nullptr) {
		auto* Set=B.ActionCall(GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,SetParameter),CurrentAction);
		Set->FindPinChecked(TEXT("Name"))->DefaultValue=Name;
		Set->FindPinChecked(TEXT("Location"))->DefaultValue=StaticEnum<EVaRestEndpointParamLocation>()->GetNameStringByValue(static_cast<int64>(Location));
		Set->FindPinChecked(TEXT("Style"))->DefaultValue=StaticEnum<EVaRestQueryStyle>()->GetNameStringByValue(static_cast<int64>(Style));
		auto* Value=Set->FindPinChecked(TEXT("Value"));
		const bool bRef=Value->PinType.bIsReference,bConst=Value->PinType.bIsConst;
		Value->PinType=Input->PinType; Value->PinType.bIsReference=bRef; Value->PinType.bIsConst=bConst;
		if (Defaults)
		{
			auto* Array=B.Node<UK2Node_MakeArray>();
			const auto* Count=FindFProperty<FIntProperty>(UK2Node_MakeContainer::StaticClass(),TEXT("NumInputs"));
			if (!Count) { B.bValid=false;Compiler.MessageLog.Error(TEXT("@@ cannot construct this SDK's array default node."),&Source);return; }
			Count->SetPropertyValue_InContainer(Array,Defaults->Num());Array->AllocateDefaultPins();B.Link(Array->GetOutputPin(),Value);Array->NotifyPinConnectionListChanged(Array->GetOutputPin());
			for (int32 Index=0;Index<Defaults->Num();++Index)
			{
				auto* Element=Array->FindPin(Array->GetPinName(Index));if (!Element) { B.bValid=false;continue; }
				Element->DefaultValue=(*Defaults)[Index];
				const FString Invalid=B.Schema->IsPinDefaultValid(Element,Element->DefaultValue,nullptr,FText::GetEmpty());
				if (!Invalid.IsEmpty()) { B.bValid=false;Compiler.MessageLog.Error(*FString::Printf(TEXT("@@ %s default[%d]: %s"),*Name,Index,*Invalid),&Source); }
			}
		}
		else B.MoveInput(Input,Value);
		B.Link(Exec,Set->GetExecPin());Exec=Set->GetThenPin();
	};
	if (Endpoint)
	{
		for (const auto& Param:Endpoint->Params)
		{
			auto* Input=Source.FindPin(UK2Node_VaRestRequestBase::ParameterPin(Param.PinName));
			if (Param.bRemoved || !Input) continue;
			if (!Param.bRequired && Input->LinkedTo.IsEmpty() && UK2Node_VaRestRequestBase::IsAtAutogeneratedDefault(*Input)) continue;
			if (Param.Type.Container==EVaRestTypeContainer::Array && Input->LinkedTo.IsEmpty())
			{
				TArray<FString> Values;FString Invalid;
				if (!ArrayParameterDefaults(Param.Type,Param.Default,Values,Invalid)) { B.bValid=false;Compiler.MessageLog.Error(*FString::Printf(TEXT("@@ %s: %s"),*Param.Name,*Invalid),&Source);continue; }
				Parameter(Input,Param.Name,Param.In,Param.Style,&Values);
			}
			else Parameter(Input,Param.Name,Param.In,Param.Style); // wire spelling is never the FName pin identity
		}
		for (const auto& Name:Source.UndeclaredPaths()) Parameter(Source.FindPin(UK2Node_VaRestRequestBase::PathPin(Name)),Name,EVaRestEndpointParamLocation::Path,EVaRestQueryStyle::Csv);
	}
	auto* BodyJson=Source.FindPin(TEXT("BodyJson"));auto* Body=BodyJson && !BodyJson->LinkedTo.IsEmpty() ? BodyJson : Source.FindPin(TEXT("Body"));
	if (Body && (!Body->LinkedTo.IsEmpty() || !UK2Node_VaRestRequestBase::IsAtAutogeneratedDefault(*Body) || Body->DefaultObject))
	{
		EVaRestEndpointBodyKind Kind=Endpoint ? Endpoint->BodyKind : EVaRestEndpointBodyKind::Json;
		if (!Endpoint)
		{
			if (Body->PinType.PinCategory==UEdGraphSchema_K2::PC_String) Kind=EVaRestEndpointBodyKind::Text;
			else if (Body->PinType.PinCategory==UEdGraphSchema_K2::PC_Byte && Body->PinType.ContainerType==EPinContainerType::Array) Kind=EVaRestEndpointBodyKind::Binary;
			else if (Body->PinType.PinSubCategoryObject==FVaRestMultipartData::StaticStruct()) Kind=EVaRestEndpointBodyKind::Multipart;
		}
		auto* Set=B.ActionCall(GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,SetRequestBody),CurrentAction);Set->FindPinChecked(TEXT("Kind"))->DefaultValue=StaticEnum<EVaRestEndpointBodyKind>()->GetNameStringByValue(static_cast<int64>(Kind));
		Set->FindPinChecked(TEXT("bBytesAsBase64"))->DefaultValue=Endpoint && Body!=BodyJson && Endpoint->RequestType.Kind==EVaRestTypeKind::Bytes && Endpoint->RequestType.bJsonValue ? TEXT("true") : TEXT("false");
		if (Endpoint) Set->FindPinChecked(TEXT("ContentType"))->DefaultValue=Endpoint->BodyContentType;
		auto* Value=Set->FindPinChecked(TEXT("Value"));
		const bool bRef=Value->PinType.bIsReference,bConst=Value->PinType.bIsConst;
		Value->PinType=Body->PinType; Value->PinType.bIsReference=bRef; Value->PinType.bIsConst=bConst;
		B.MoveInput(Body,Value);B.Link(Exec,Set->GetExecPin());Exec=Set->GetThenPin();
	}
	const auto Transfer=ExpandRequestTransfer(Graph,&Source,Source.NodeGuid,CurrentAction,bTypedResponse ? &ResponsePin : nullptr,bTypedError ? &ErrorPin : nullptr,&Compiler);
	B.bValid=Transfer.bValid && B.bValid;B.Link(Exec,Transfer.Bind->GetExecPin());
	auto* Activate=B.ActionCall(GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,Activate),CurrentAction);B.Link(Transfer.BindingThen,Activate->GetExecPin());
	auto* Handle=B.Variable(PinType(UEdGraphSchema_K2::PC_Object,UVaRestRequestHandle::StaticClass()),TEXT("Handle"));
	auto* Http=B.Variable(PinType(UEdGraphSchema_K2::PC_Struct,FVaRestResponseData::StaticStruct()),TEXT("Http"));
	auto* Error=B.Variable(PinType(UEdGraphSchema_K2::PC_Struct,FVaRestError::StaticStruct()),TEXT("Error"));
	auto* Reason=B.Variable(PinType(UEdGraphSchema_K2::PC_Byte,StaticEnum<EVaRestTypedOutputUnavailableReason>()),TEXT("UnavailableReason"));
	auto* Queue=B.Variable(PinType(UEdGraphSchema_K2::PC_String),TEXT("Queue"));
	auto* Response=bTypedResponse ? Transfer.Response : ResponseType.Kind!=EVaRestTypeKind::None ? B.Variable(ResponsePin,TEXT("Response")) : nullptr;
	auto* ErrorBody=bTypedError ? Transfer.ErrorBody : ErrorType.Kind!=EVaRestTypeKind::None ? B.Variable(ErrorPin,TEXT("ErrorBody")) : nullptr;
	auto* ErrorValid=Transfer.ErrorValid;
	Exec=Activate->GetThenPin();B.Assign(Exec,Handle,B.Getter(GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,GetHandle),CurrentAction));
	UEdGraphPin* CaptureThen=Exec;
	// Each callback publishes its own Action/RequestId metadata before any user execution.
	const auto Metadata=[&](UEdGraphPin*& Path,UK2Node_CustomEvent* Event,bool bUnavailable=false) {
		B.Assign(Path,Handle,B.Getter(GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,GetHandle),Event->FindPin(TEXT("Action"))));
		B.Assign(Path,Http,Event->FindPin(TEXT("Http")));B.Assign(Path,Error,Event->FindPin(bUnavailable ? TEXT("OriginalError") : TEXT("Error")));
	};
	UEdGraphPin* Typed=Transfer.AcceptedExec;Metadata(Typed,Transfer.Continue);
	auto* TypedBranch=B.Branch(Typed,Transfer.Continue->FindPin(TEXT("bSuccess")));
	UEdGraphPin* Raw=Transfer.Metadata->GetThenPin();Metadata(Raw,Transfer.Metadata);
	auto* RawBranch=B.Branch(Raw,Transfer.Metadata->FindPin(TEXT("bSuccess")));
	UEdGraphPin* RawSuccess=RawBranch->GetThenPin();
	if (Response && !bTypedResponse) B.Assign(RawSuccess,Response,B.Getter(RawGetter(ResponseType,false),Transfer.Metadata->FindPin(TEXT("Action"))));
	UEdGraphPin* RawError=RawBranch->GetElsePin();
	if (ErrorBody && !bTypedError)
	{
		B.Assign(RawError,ErrorBody,B.Getter(RawGetter(ErrorType,true),Transfer.Metadata->FindPin(TEXT("Action"))));
		B.Assign(RawError,ErrorValid,B.Getter(ErrorType.Kind==EVaRestTypeKind::Json ? GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,HasErrorJson) :
			GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,HasErrorBody),Transfer.Metadata->FindPin(TEXT("Action"))));
	}
	else B.Assign(RawError,ErrorValid,nullptr,TEXT("false"));
	UEdGraphPin* Unavailable=Transfer.Unavailable->GetThenPin();Metadata(Unavailable,Transfer.Unavailable,true);
	B.Assign(Unavailable,Reason,Transfer.Unavailable->FindPin(TEXT("Reason")));
	UEdGraphPin* Queued=Transfer.Queued->GetThenPin();B.Assign(Queued,Handle,B.Getter(GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,GetHandle),Transfer.Queued->FindPin(TEXT("Action"))));
	B.Assign(Queued,Http,B.Getter(GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,GetTerminalResponse),Transfer.Queued->FindPin(TEXT("Action"))));B.Assign(Queued,Queue,Transfer.Queued->FindPin(TEXT("QueueId")));
	// Compiler wiring copies both mutually exclusive native predecessors before removing the original node links.
	const auto Routes=[&](FName Name,UEdGraphPin* First,UEdGraphPin* Second=nullptr) {
		if (auto* Original=Source.FindPin(Name))
		{
			if (First) Compiler.CopyPinLinksToIntermediate(*Original,*First);
			if (Second) Compiler.CopyPinLinksToIntermediate(*Original,*Second);
			Original->BreakAllPinLinks();
		}
	};
	Routes(TEXT("OnSuccess"),TypedBranch->GetThenPin(),RawSuccess);
	// No action was admitted: publish explicit Config with RequestId 0 and a null Handle, before any Send.
	auto* Failure=B.Call(UVaRestRequestAction::StaticClass(),GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,AdmissionError));B.Link(Factory->FindPin(TEXT("Error")),Failure->FindPin(TEXT("Detail")));B.Link(Tag->GetVariablePin(),Failure->FindPin(TEXT("Tag")));
	UEdGraphPin* Refused=Admitted->GetElsePin();B.Assign(Refused,Handle,nullptr,TEXT(""));B.Assign(Refused,Http,Failure->FindPin(TEXT("Http")));B.Assign(Refused,Error,Failure->GetReturnValuePin());B.Assign(Refused,ErrorValid,nullptr,TEXT("false"));
	auto* Refusal=B.Node<UK2Node_ExecutionSequence>();Refusal->AllocateDefaultPins();B.Link(Refused,Refusal->GetExecPin());
	// Then is an immediate capture/admission signal on both branches; the refusal is an explicit pre-request error.
	Routes(UEdGraphSchema_K2::PN_Then,CaptureThen,Refusal->GetThenPinGivenIndex(0));
	if (auto* Original=Source.FindPin(TEXT("OnError")))
	{
		Compiler.CopyPinLinksToIntermediate(*Original,*TypedBranch->GetElsePin());Compiler.CopyPinLinksToIntermediate(*Original,*RawError);
		Compiler.MovePinLinksToIntermediate(*Original,*Refusal->GetThenPinGivenIndex(1));
	}
	Routes(TEXT("OnTypedOutputUnavailable"),Unavailable);Routes(TEXT("OnQueued"),Queued);
	B.Output(TEXT("Handle"),Handle->GetVariablePin());B.Output(TEXT("Http"),Http->GetVariablePin());B.Output(TEXT("Error"),Error->GetVariablePin());
	B.Output(TEXT("UnavailableReason"),Reason->GetVariablePin());B.Output(TEXT("QueueId"),Queue->GetVariablePin());
	if (Response) B.Output(TEXT("Response"),Response->GetVariablePin());if (ErrorBody) B.Output(TEXT("ErrorBody"),ErrorBody->GetVariablePin());B.Output(TEXT("ErrorBodyValid"),ErrorValid->GetVariablePin());
	if (!B.bValid) Compiler.MessageLog.Error(TEXT("@@ could not construct its validated request/transfer graph."),&Source);
	Source.BreakAllNodeLinks();
}
}
