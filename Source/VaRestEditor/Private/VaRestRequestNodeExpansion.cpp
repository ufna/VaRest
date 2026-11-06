// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestRequestNodeExpansion.h"
#include "VaRestRequestAction.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_AssignmentStatement.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Self.h"
#include "K2Node_TemporaryVariable.h"
#include "KismetCompiler.h"
#include "UObject/UnrealType.h"

namespace VaRest::Http::Editor
{
namespace RequestExpansionPrivate
{
struct FBuilder
{
	UEdGraph* Graph;
	UK2Node* Source;
	FKismetCompilerContext* Compiler;
	const UEdGraphSchema_K2* Schema=GetDefault<UEdGraphSchema_K2>();
	FGuid Identity;
	uint32 Role=0;
	bool bValid=true;
	const FPreparedTransferContract* Contract=nullptr;
	template <typename T> T* Node()
	{
		auto* Result=Compiler ? Compiler->SpawnIntermediateNode<T>(Source,Graph) : NewObject<T>(Graph);
		if (!Compiler) Graph->AddNode(Result,false,false);
		Result->NodeGuid=FGuid::Combine(Identity,FGuid(0x56415232,0,0,++Role));
		return Result;
	}
	void Link(UEdGraphPin* A,UEdGraphPin* B) { bValid=bool(A && B && Schema->TryCreateConnection(A,B)) && bValid; }
	UK2Node_CallFunction* Call(FName Function)
	{
		auto* Result=Node<UK2Node_CallFunction>();
		Result->SetFromFunction(Contract->ActionClass->FindFunctionByName(Function)); Result->AllocateDefaultPins(); return Result;
	}
	UK2Node_TemporaryVariable* Variable(const FEdGraphPinType& Type,const TCHAR* Name)
	{
		auto* Result=Node<UK2Node_TemporaryVariable>(); Result->VariableType=Type;
		Result->NodeComment=FString(Name)+Identity.ToString(EGuidFormats::Digits);
		Result->bIsPersistent=false; // shared storage is TS_ForcedShared EventGraphLocals, not CPF_SaveGame
		Result->AllocateDefaultPins(); return Result;
	}
	void Assign(UEdGraphPin*& Exec,UK2Node_TemporaryVariable* Variable,UEdGraphPin* Value)
	{
		auto* Result=Node<UK2Node_AssignmentStatement>(); Result->AllocateDefaultPins();
		Link(Exec,Result->GetExecPin()); Link(Variable->GetVariablePin(),Result->GetVariablePin());
		Result->NotifyPinConnectionListChanged(Result->GetVariablePin());
		Link(Value,Result->GetValuePin()); Result->NotifyPinConnectionListChanged(Result->GetValuePin()); Exec=Result->GetThenPin();
	}
	UK2Node_CustomEvent* Event(FStringView Prefix,FName DelegateParameter,UK2Node_CallFunction* Bind,UK2Node_Self* Self)
	{
		auto* Result=Node<UK2Node_CustomEvent>();
		Result->CustomFunctionName=FName(FString(Prefix)+Identity.ToString(EGuidFormats::Digits));
		auto* Function=Contract->ActionClass->FindFunctionByName(Bind->FunctionReference.GetMemberName());
		auto* Signature=Function ? CastField<FDelegateProperty>(Function->FindPropertyByName(DelegateParameter)) : nullptr;
		if (!Signature || !Signature->SignatureFunction) { Result->AllocateDefaultPins(); bValid=false; return Result; }
		Result->SetDelegateSignature(Signature->SignatureFunction);
		Result->AllocateDefaultPins();
		auto* Delegate=Node<UK2Node_CreateDelegate>(); Delegate->AllocateDefaultPins();
		Link(Delegate->GetDelegateOutPin(),Bind->FindPin(DelegateParameter));
		Link(Self->FindPin(UEdGraphSchema_K2::PN_Self),Delegate->GetObjectInPin()); Delegate->SetFunction(Result->GetFunctionName());
		return Result;
	}
};
}

FRequestTransferExpansion ExpandPreparedTransfer(UEdGraph* Graph,UK2Node* Source,const FGuid& NodeId,UEdGraphPin* Action,
	const FEdGraphPinType* ResponseType,const FEdGraphPinType* ErrorType,const FPreparedTransferContract& Contract,FKismetCompilerContext* Compiler)
{
	FRequestTransferExpansion Result;
	if (!Graph || !Source || !NodeId.IsValid() || !Action || !IsValid(Contract.ActionClass)) return Result;
	const auto Has=[&](FName Name) { return !Name.IsNone() && Contract.ActionClass->FindFunctionByName(Name); };
	if (!Has(Contract.BindOutput) || !Has(Contract.ConfirmPrepared) || !Has(Contract.AcceptPreparedOutput)
		|| (ResponseType && !Has(Contract.TakeResponse)) || (ErrorType && !Has(Contract.TakeErrorBody))
		|| (!Contract.BindMetadata.IsNone() && !Has(Contract.BindMetadata)) || (!Contract.BindQueued.IsNone() && !Has(Contract.BindQueued))) return Result;
	RequestExpansionPrivate::FBuilder B{Graph,Source,Compiler,GetDefault<UEdGraphSchema_K2>(),NodeId};
	B.Contract=&Contract;
	auto* Self=B.Node<UK2Node_Self>(); Self->AllocateDefaultPins();
	Result.Bind=B.Call(Contract.BindOutput);
	B.Link(Action,Result.Bind->FindPin(UEdGraphSchema_K2::PN_Self));
	Result.Prepare=B.Event(TEXTVIEW("__VaRest_Prepare_"),TEXT("Prepare"),Result.Bind,Self);
	Result.Continue=B.Event(TEXTVIEW("__VaRest_Continue_"),TEXT("Continue"),Result.Bind,Self);
	Result.Unavailable=B.Event(TEXTVIEW("__VaRest_Unavailable_"),TEXT("Unavailable"),Result.Bind,Self);
	Result.BindingThen=Result.Bind->GetThenPin();
	if (!Contract.BindMetadata.IsNone())
	{
		auto* BindMetadata=B.Call(Contract.BindMetadata);
		B.Link(Action,BindMetadata->FindPin(UEdGraphSchema_K2::PN_Self)); B.Link(Result.BindingThen,BindMetadata->GetExecPin());
		Result.Metadata=B.Event(TEXTVIEW("__VaRest_Metadata_"),TEXT("Metadata"),BindMetadata,Self); Result.BindingThen=BindMetadata->GetThenPin();
	}
	if (!Contract.BindQueued.IsNone())
	{
		auto* BindQueued=B.Call(Contract.BindQueued);
		B.Link(Action,BindQueued->FindPin(UEdGraphSchema_K2::PN_Self)); B.Link(Result.BindingThen,BindQueued->GetExecPin());
		Result.Queued=B.Event(TEXTVIEW("__VaRest_Queued_"),TEXT("Queued"),BindQueued,Self); Result.BindingThen=BindQueued->GetThenPin();
	}
	const FEdGraphPinType Bool(UEdGraphSchema_K2::PC_Boolean,NAME_None,nullptr,EPinContainerType::None,false,FEdGraphTerminalType());
	const FEdGraphPinType String(UEdGraphSchema_K2::PC_String,NAME_None,nullptr,EPinContainerType::None,false,FEdGraphTerminalType());
	Result.Cookie=B.Variable(String,TEXT("VaRestCookie_"));
	Result.ResponseValid=B.Variable(Bool,TEXT("VaRestResponseValid_")); Result.ErrorValid=B.Variable(Bool,TEXT("VaRestErrorValid_"));
	UEdGraphPin* Exec=Result.Prepare->GetThenPin();
	const auto Take=[&](const FEdGraphPinType* Type,FName Function,const TCHAR* Name,UK2Node_TemporaryVariable*& Storage,UK2Node_TemporaryVariable* Validity) {
		if (!Type) return;
		Storage=B.Variable(*Type,Name);
		auto* Call=B.Call(Function); Call->FindPinChecked(TEXT("Value"))->PinType=*Type;
		B.Link(Exec,Call->GetExecPin()); B.Link(Result.Prepare->FindPin(TEXT("Action")),Call->FindPin(UEdGraphSchema_K2::PN_Self));
		Exec=Call->GetThenPin(); B.Assign(Exec,Storage,Call->FindPin(TEXT("Value"))); B.Assign(Exec,Validity,Call->GetReturnValuePin());
	};
	Take(ResponseType,Contract.TakeResponse,TEXT("VaRestResponse_"),Result.Response,Result.ResponseValid);
	Take(ErrorType,Contract.TakeErrorBody,TEXT("VaRestError_"),Result.ErrorBody,Result.ErrorValid);
	B.Assign(Exec,Result.Cookie,Result.Prepare->FindPin(TEXT("RequestIdText")));
	auto* Confirm=B.Call(Contract.ConfirmPrepared);
	B.Link(Exec,Confirm->GetExecPin()); B.Link(Result.Prepare->FindPin(TEXT("Action")),Confirm->FindPin(UEdGraphSchema_K2::PN_Self));
	B.Link(Result.Prepare->FindPin(TEXT("RequestIdText")),Confirm->FindPin(TEXT("RequestIdText")));
	// Confirm.Then deliberately has no user execution connection.
	auto* Accept=B.Call(Contract.AcceptPreparedOutput);
	B.Link(Result.Continue->GetThenPin(),Accept->GetExecPin()); B.Link(Result.Continue->FindPin(TEXT("Action")),Accept->FindPin(UEdGraphSchema_K2::PN_Self));
	B.Link(Result.Cookie->GetVariablePin(),Accept->FindPin(TEXT("StoredCookie")));
	B.Link(Result.Continue->FindPin(TEXT("RequestIdText")),Accept->FindPin(TEXT("RequestIdText")));
	auto* Branch=B.Node<UK2Node_IfThenElse>(); Branch->AllocateDefaultPins();
	B.Link(Accept->GetThenPin(),Branch->GetExecPin()); B.Link(Accept->GetReturnValuePin(),Branch->GetConditionPin());
	Result.AcceptedExec=Branch->GetThenPin(); Result.bValid=B.bValid; return Result;
}
FRequestTransferExpansion ExpandRequestTransfer(UEdGraph* Graph,UK2Node* Source,const FGuid& NodeId,UEdGraphPin* Action,
	const FEdGraphPinType* ResponseType,const FEdGraphPinType* ErrorType,FKismetCompilerContext* Compiler)
{
	FPreparedTransferContract Contract; Contract.ActionClass=UVaRestRequestAction::StaticClass();
	Contract.BindOutput=GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,BindOutput);
	Contract.TakeResponse=GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,TakeResponse);
	Contract.TakeErrorBody=GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,TakeErrorBody);
	Contract.ConfirmPrepared=GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,ConfirmPrepared);
	Contract.AcceptPreparedOutput=GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,AcceptPreparedOutput);
	Contract.BindMetadata=GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,BindMetadata);
	Contract.BindQueued=GET_FUNCTION_NAME_CHECKED(UVaRestRequestAction,BindQueued);
	return ExpandPreparedTransfer(Graph,Source,NodeId,Action,ResponseType,ErrorType,Contract,Compiler);
}
}
