// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestProtocolNodeExpansion.h"
#include "VaRestPreparedOutputAction.h"
#include "VaRestRequestNodeExpansion.h"
#include "VaRestTypeRefDetails.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_AssignmentStatement.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_TemporaryVariable.h"
#include "KismetCompiler.h"

namespace VaRestEditor
{
bool ExpandProtocolOutputBinding(UK2Node* Source, UEdGraph* Graph, FKismetCompilerContext& Compiler,
	UEdGraphPin* Action, UEdGraphPin*& BindingThen, const FVaRestTypeRef& Type,
	const TMap<FName, UK2Node_TemporaryVariable*>& Outputs, const FProtocolTransferConfig& Config)
{
	const auto* Schema = GetDefault<UEdGraphSchema_K2>(); bool bValid = true;
	const auto Link = [&](UEdGraphPin* A, UEdGraphPin* B) { bValid = bool(A && B && Schema->TryCreateConnection(A, B)) && bValid; };
	const auto Call = [&](UClass* Class, FName Name)
	{
		auto* Node = Compiler.SpawnIntermediateNode<UK2Node_CallFunction>(Source, Graph);
		Node->SetFromFunction(Class->FindFunctionByName(Name)); Node->AllocateDefaultPins(); return Node;
	};
	const auto Assign = [&](UEdGraphPin*& Exec, UK2Node_TemporaryVariable* Storage, UEdGraphPin* Value)
	{
		if (!Storage || !Value) { bValid = false; return; }
		auto* Node = Compiler.SpawnIntermediateNode<UK2Node_AssignmentStatement>(Source, Graph); Node->AllocateDefaultPins();
		Link(Exec, Node->GetExecPin()); Link(Storage->GetVariablePin(), Node->GetVariablePin()); Node->NotifyPinConnectionListChanged(Node->GetVariablePin());
		Link(Value, Node->GetValuePin()); Node->NotifyPinConnectionListChanged(Node->GetValuePin()); Exec = Node->GetThenPin();
	};
	FEdGraphPinType PinType; FString Detail;
	if (!VaRest::Http::Editor::TypeToPin(Type, PinType, Detail)) return false;
	auto* Configure = Call(UVaRestPreparedOutputAction::StaticClass(), GET_FUNCTION_NAME_CHECKED(UVaRestPreparedOutputAction, ConfigureOutputType));
	Link(Action, Configure->FindPin(UEdGraphSchema_K2::PN_Self)); Link(BindingThen, Configure->GetExecPin());
	auto* Literal = Compiler.SpawnIntermediateNode<UK2Node_MakeStruct>(Source, Graph); Literal->StructType = FVaRestTypeRef::StaticStruct();
	// This literal is newly generated, so it has no legacy override pins to migrate (as with a newly placed MakeStruct).
	Literal->bMadeAfterOverridePinRemoval = true; Literal->AllocateDefaultPins();
	Literal->FindPinChecked(TEXT("Kind"))->DefaultValue = StaticEnum<EVaRestTypeKind>()->GetNameStringByValue(static_cast<int64>(Type.Kind));
	Literal->FindPinChecked(TEXT("Container"))->DefaultValue = StaticEnum<EVaRestTypeContainer>()->GetNameStringByValue(static_cast<int64>(Type.Container));
	Literal->FindPinChecked(TEXT("bJsonValue"))->DefaultValue = Type.bJsonValue ? TEXT("true") : TEXT("false"); Literal->FindPinChecked(TEXT("Type"))->DefaultObject = Type.Type.Get();
	UEdGraphPin* TypeLiteral = nullptr;
	for (auto* Pin : Literal->Pins) if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct) TypeLiteral = Pin;
	Link(TypeLiteral, Configure->FindPin(TEXT("Type")));
	VaRest::Http::Editor::FPreparedTransferContract Contract; Contract.ActionClass = UVaRestPreparedOutputAction::StaticClass();
	Contract.BindOutput = GET_FUNCTION_NAME_CHECKED(UVaRestPreparedOutputAction, BindOutput);
	Contract.TakeResponse = GET_FUNCTION_NAME_CHECKED(UVaRestPreparedOutputAction, TakeResponse);
	Contract.ConfirmPrepared = GET_FUNCTION_NAME_CHECKED(UVaRestPreparedOutputAction, ConfirmPrepared);
	Contract.AcceptPreparedOutput = GET_FUNCTION_NAME_CHECKED(UVaRestPreparedOutputAction, AcceptPreparedOutput);
	const auto Transfer = VaRest::Http::Editor::ExpandPreparedTransfer(Graph, Source, Source->NodeGuid, Action, &PinType, nullptr, Contract, &Compiler);
	if (!Transfer.bValid) return false;
	Link(Configure->GetThenPin(), Transfer.Bind->GetExecPin()); BindingThen = Transfer.BindingThen;
	// Accept consumes the per-delivery cookie before any native metadata assignment or user execution.
	UEdGraphPin* Success = Transfer.AcceptedExec;
	Assign(Success, Outputs.FindRef(Config.TypedOutput), Transfer.Response->GetVariablePin());
	if (auto* Error = Outputs.FindRef(TEXT("Error"))) Assign(Success, Error, Transfer.Continue->FindPin(TEXT("Error")));
	if (auto* Warnings = Outputs.FindRef(TEXT("DecodeWarnings")))
	{
		auto* Getter = Call(UVaRestPreparedOutputAction::StaticClass(), GET_FUNCTION_NAME_CHECKED(UVaRestPreparedOutputAction, GetPreparedDecodeWarnings));
		Link(Transfer.Continue->FindPin(TEXT("Action")), Getter->FindPin(TEXT("Action")));
		Link(Transfer.Continue->FindPin(TEXT("RequestIdText")), Getter->FindPin(TEXT("RequestIdText")));
		Assign(Success, Warnings, Getter->GetReturnValuePin());
	}
	auto* Metadata = Call(Config.MetadataClass, Config.MetadataGetter);
	Link(Transfer.Continue->FindPin(TEXT("Action")), Metadata->FindPin(TEXT("Action")));
	Link(Transfer.Continue->FindPin(TEXT("RequestIdText")), Metadata->FindPin(TEXT("RequestIdText")));
	UK2Node_BreakStruct* Fields = nullptr;
	if (Config.bSplitMetadata)
	{
		Fields = Compiler.SpawnIntermediateNode<UK2Node_BreakStruct>(Source, Graph);
		Fields->StructType = CastChecked<UScriptStruct>(Metadata->GetReturnValuePin()->PinType.PinSubCategoryObject.Get());
		Fields->bMadeAfterOverridePinRemoval = true; // also a new node, with no serialized legacy override pins
		Fields->AllocateDefaultPins(); UEdGraphPin* Input = nullptr;
		for (auto* Pin : Fields->Pins) if (Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct) Input = Pin;
		Link(Metadata->GetReturnValuePin(), Input);
	}
	for (const auto& Pair : Outputs)
	{
		if (Pair.Key == Config.TypedOutput || Pair.Key == TEXT("Error") || Pair.Key == TEXT("DecodeWarnings")) continue;
		if (Pair.Key == Config.MetadataOutput) Assign(Success, Pair.Value, Metadata->GetReturnValuePin());
		else if (Fields) if (auto* Field = Fields->FindPin(Pair.Key, EGPD_Output)) Assign(Success, Pair.Value, Field);
	}
	if (auto* Pin = Source->FindPin(Config.SuccessExec, EGPD_Output))
		bValid = Compiler.MovePinLinksToIntermediate(*Pin, *Success).CanSafeConnect() && bValid;
	UEdGraphPin* Unavailable = Transfer.Unavailable->GetThenPin();
	if (auto* Error = Outputs.FindRef(TEXT("Error"))) Assign(Unavailable, Error, Transfer.Unavailable->FindPin(TEXT("OriginalError")));
	if (auto* Pin = Source->FindPin(GET_MEMBER_NAME_CHECKED(UVaRestPreparedOutputAction, OnTypedOutputUnavailable), EGPD_Output))
		bValid = Compiler.MovePinLinksToIntermediate(*Pin, *Unavailable).CanSafeConnect() && bValid;
	return bValid;
}
}
