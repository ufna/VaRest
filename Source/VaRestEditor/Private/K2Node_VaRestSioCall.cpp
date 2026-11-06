// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "K2Node_VaRestSioCall.h"
#include "VaRestApi.h"
#include "VaRestSioClient.h"
#include "VaRestSioLibrary.h"
#include "VaRestSioNodeSpawner.h"
#include "VaRestTypeRefDetails.h"
#include "BlueprintActionDatabaseRegistrar.h"
#include "EdGraphSchema_K2.h"
#include "KismetCompiler.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"

namespace VaRestEditor::SioNodes
{
	const UVaRestSioEndpoint* ResolveEntry(const UVaRestApi* Api, FName Entry)
	{
		const UVaRestSioEndpoint* Result = nullptr; bool bFound = false;
		if (IsValid(Api)) for (const auto& Candidate : Api->Sockets) if (IsValid(Candidate.Get()) && Candidate->Name == Entry)
		{ if (bFound) return nullptr; bFound = true; Result = Cast<UVaRestSioEndpoint>(Candidate.Get()); }
		return Result;
	}
	const FVaRestSioEventDeclaration* ResolveDeclaration(const UVaRestSioEndpoint* Endpoint, FStringView Namespace, FStringView Event, EVaRestSioCall Operation)
	{
		if (!Endpoint) return nullptr;
		const auto Direction = Operation == EVaRestSioCall::Emit ? EVaRestSioEventDirection::Out
			: (Operation == EVaRestSioCall::EmitWithAck ? EVaRestSioEventDirection::OutWithAck
				: (Operation == EVaRestSioCall::ListenWithAck ? EVaRestSioEventDirection::InWithAck : EVaRestSioEventDirection::In));
		for (const auto& Row : Endpoint->Namespaces) if (Namespace.Equals(Row.Name, ESearchCase::CaseSensitive))
			return Row.Events.FindByPredicate([&](const auto& Value) { return Value.Direction == Direction && Event.Equals(Value.Event, ESearchCase::CaseSensitive); });
		return nullptr;
	}
	bool PayloadPinType(const FVaRestTypeRef& Type, FEdGraphPinType& PinType, FString& Error)
	{ return VaRest::Http::Editor::TypeToPin(Type.Kind == EVaRestTypeKind::None ? FVaRestTypeRef::Json() : Type, PinType, Error); }
	void ValidateIdentity(const UK2Node& Node, const UVaRestSioEndpoint* Endpoint, bool bAssetEntry, bool bDeclaration, const FVaRestSioEventDeclaration* Declaration, FCompilerResultsLog& Log)
	{
		if (bAssetEntry)
		{
			if (!Endpoint) { Log.Error(TEXT("@@ has a missing or ambiguous Socket.IO entry; its pins have been retained."), &Node); return; }
			FVaRestApiValidationContext Validation; Endpoint->Validate(Validation);
			for (const auto& Issue : Validation.Issues) if (!Issue.bWarning) Log.Error(*FString::Printf(TEXT("@@ %s: %s"), *Issue.PropertyPath, *Issue.Message.ToString()), &Node);
			if (bDeclaration && !Declaration) Log.Error(TEXT("@@ has a missing event declaration; restore the exact namespace, event and direction."), &Node);
		}
		for (const auto* Pin : Node.Pins) if (Pin->bOrphanedPin && !Pin->LinkedTo.IsEmpty()) Log.Error(TEXT("@@ has a linked removed/incompatible pin; reconnect or remove it."), &Node);
	}
}

UK2Node_VaRestSioCall::UK2Node_VaRestSioCall() { OrphanedPinSaveMode = ESaveOrphanPinMode::SaveAll; }
FName UK2Node_VaRestSioCall::ArgumentPinName(int32 Index) { return FName(*FString::Printf(TEXT("Arg%d"), Index)); }
const UVaRestSioEndpoint* UK2Node_VaRestSioCall::ResolveEntry() const { return VaRestEditor::SioNodes::ResolveEntry(Api, Entry); }
const FVaRestSioEventDeclaration* UK2Node_VaRestSioCall::ResolveDeclaration() const { return VaRestEditor::SioNodes::ResolveDeclaration(ResolveEntry(), Namespace, Event, Operation); }
void UK2Node_VaRestSioCall::AllocateDefaultPins()
{
	PinError.Reset();
	if (Operation == EVaRestSioCall::Get) SetFromFunction(UVaRestSioLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UVaRestSioLibrary, GetSocketIO)));
	else if (Operation == EVaRestSioCall::Reply) SetFromFunction(UVaRestSioLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UVaRestSioLibrary, AckReply)));
	else if (bAssetEntry) SetFromFunction(UVaRestSioLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UVaRestSioLibrary, EmitAssetEvent)));
	else SetFromFunction(UVaRestSioClient::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UVaRestSioClient, EmitArguments)));
	Super::AllocateDefaultPins();
	if (IsVariadic())
	{
		ArgumentCount = FMath::Clamp(ArgumentCount, 0, 32);
		if (auto* Pin = FindPin(TEXT("ArgumentCount"))) { Pin->DefaultValue = LexToString(ArgumentCount); Pin->bHidden = true; }
		for (int32 Index = 0; Index < 32; ++Index) if (auto* Pin = FindPin(ArgumentPinName(Index)))
		{
			Pin->bHidden = Index >= ArgumentCount; Pin->PinFriendlyName = FText::FromString(FString::Printf(TEXT("Arg %d"), Index + 1));
			if (Pin->bHidden)
			{ Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Int; Pin->PinType.PinSubCategory = NAME_None; Pin->PinType.PinSubCategoryObject = nullptr; Pin->DefaultValue = TEXT("0"); }
		}
	}
	if (bAssetEntry)
	{
		if (auto* Pin = FindPin(TEXT("Api"))) { Pin->DefaultObject = Api; Pin->bHidden = true; }
		if (auto* Pin = FindPin(TEXT("Entry"))) { Pin->DefaultValue = Entry.ToString(); Pin->bHidden = true; }
	}
	if (bDeclaration)
	{
		if (auto* Pin = FindPin(TEXT("Event"))) { Pin->DefaultValue = Event; Pin->bHidden = true; }
		if (auto* Pin = FindPin(TEXT("Namespace"))) { Pin->DefaultValue = Namespace; Pin->bHidden = true; }
		if (const auto* Declaration = ResolveDeclaration())
		{
			if (auto* Pin = FindPin(TEXT("ArgsMode"))) { Pin->DefaultValue = StaticEnum<EVaRestSioArgs>()->GetNameStringByValue(static_cast<int64>(Declaration->ArgsMode)); Pin->bHidden = true; }
			FEdGraphPinType Type;
			if (VaRestEditor::SioNodes::PayloadPinType(Declaration->PayloadType, Type, PinError)) if (auto* Pin = FindPin(TEXT("Payload")))
			{ Type.bIsReference = Pin->PinType.bIsReference; Type.bIsConst = Pin->PinType.bIsConst; Pin->PinType = Type; GetDefault<UEdGraphSchema_K2>()->SetPinAutogeneratedDefaultValueBasedOnType(Pin); }
		}
	}
}
void UK2Node_VaRestSioCall::ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins)
{
	for (auto* Pin : OldPins) Pin->SetSavePinIfOrphaned(true);
	AllocateDefaultPins();
	if (IsVariadic()) for (const auto* Old : OldPins)
		if (Old->Direction == EGPD_Input && !Old->bHidden && !Old->ParentPin && Old->PinName.ToString().StartsWith(TEXT("Arg")))
			if (auto* Pin = FindPin(Old->PinName, EGPD_Input); Pin && !Pin->bHidden) Pin->PinType = Old->PinType;
	if (bDeclaration && !ResolveDeclaration()) for (const auto* Old : OldPins) if (!Old->ParentPin && Old->PinName == TEXT("Payload")) if (auto* Pin = FindPin(TEXT("Payload"))) Pin->PinType = Old->PinType;
	RestoreSplitPins(OldPins);
}
void UK2Node_VaRestSioCall::NotifyPinConnectionListChanged(UEdGraphPin* Pin)
{
	const FEdGraphPinType Before = Pin ? Pin->PinType : FEdGraphPinType();
	Super::NotifyPinConnectionListChanged(Pin);
	if (IsVariadic() && Pin && Pin->Direction == EGPD_Input && !Pin->bHidden && Pin->PinName.ToString().StartsWith(TEXT("Arg")))
	{
		if (Pin->LinkedTo.IsEmpty()) { Pin->PinType = FEdGraphPinType(); Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard; }
		else Pin->PinType = Pin->LinkedTo[0]->PinType;
		Pin->PinType.bIsReference = Before.bIsReference; Pin->PinType.bIsConst = Before.bIsConst;
	}
	if (!Pin || Pin->Direction != EGPD_Input || Pin->PinName != TEXT("Payload")) return;
	if (bDeclaration)
	{
		Pin->PinType = Before; // a removed declaration also retains its old pin shape
		if (const auto* Declaration = ResolveDeclaration())
		{
			FEdGraphPinType Declared; PinError.Reset();
			if (VaRestEditor::SioNodes::PayloadPinType(Declaration->PayloadType, Declared, PinError)) Pin->PinType = Declared;
		}
	}
	else if (!Pin->LinkedTo.IsEmpty()) Pin->PinType = Pin->LinkedTo[0]->PinType;
	Pin->PinType.bIsReference = Before.bIsReference; Pin->PinType.bIsConst = Before.bIsConst;
}
void UK2Node_VaRestSioCall::PostEditChangeProperty(FPropertyChangedEvent& Change)
{
	Super::PostEditChangeProperty(Change);
	if (Change.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UK2Node_VaRestSioCall, Api) || Change.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UK2Node_VaRestSioCall, Entry)) { bAssetEntry = true; ReconstructNode(); }
	else if (Change.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UK2Node_VaRestSioCall, ArgumentCount)) ReconstructNode();
}
void UK2Node_VaRestSioCall::ValidateNodeDuringCompilation(FCompilerResultsLog& Log) const
{
	Super::ValidateNodeDuringCompilation(Log);
	const auto* Declaration = ResolveDeclaration(); VaRestEditor::SioNodes::ValidateIdentity(*this, ResolveEntry(), bAssetEntry, bDeclaration, Declaration, Log);
	if (!PinError.IsEmpty()) Log.Error(*FString::Printf(TEXT("@@ %s"), *PinError), this);
	if (bDeclaration && Declaration)
	{
		FEdGraphPinType Expected; FString Error;
		if (VaRestEditor::SioNodes::PayloadPinType(Declaration->PayloadType, Expected, Error)) if (const auto* Pin = FindPin(TEXT("Payload")))
		{ Expected.bIsReference = Pin->PinType.bIsReference; Expected.bIsConst = Pin->PinType.bIsConst; if (Expected != Pin->PinType) Log.Error(TEXT("@@ payload no longer matches its declaration; reconstruct and reconnect it."), this); }
	}
}
void UK2Node_VaRestSioCall::ExpandNode(FKismetCompilerContext& Compiler, UEdGraph* Graph)
{ const int32 Before = Compiler.MessageLog.NumErrors; ValidateNodeDuringCompilation(Compiler.MessageLog); if (Before == Compiler.MessageLog.NumErrors) Super::ExpandNode(Compiler, Graph); }
void UK2Node_VaRestSioCall::GetMenuActions(FBlueprintActionDatabaseRegistrar& Registrar) const
{
	if (Registrar.IsOpenForRegistration(GetClass())) for (const auto Kind : {EVaRestSioCall::Get, EVaRestSioCall::Emit, EVaRestSioCall::Reply}) Registrar.AddBlueprintAction(GetClass(), UVaRestSioNodeSpawner::Create(nullptr, NAME_None, Kind));
}
FText UK2Node_VaRestSioCall::GetNodeTitle(ENodeTitleType::Type) const
{
	if (Operation == EVaRestSioCall::Get) return FText::FromString(bAssetEntry ? TEXT("Get Socket.IO ") + Entry.ToString() : FString(TEXT("Get Socket.IO")));
	return FText::FromString(Operation == EVaRestSioCall::Reply ? FString(TEXT("Ack Reply")) : (bDeclaration ? TEXT("Emit ") + Event : FString(TEXT("Emit Socket.IO Event"))));
}
FText UK2Node_VaRestSioCall::GetTooltipText() const
{ const auto* Declaration = ResolveDeclaration(); return FText::FromString(Declaration ? Declaration->Description : FString(TEXT("Encodes up to 32 Socket.IO arguments synchronously in one packet, including binary attachments. Add pin adds an argument; the last argument can be removed."))); }
FText UK2Node_VaRestSioCall::GetMenuCategory() const { return NSLOCTEXT("VaRestSioCall", "Category", "VaRest|Socket.IO"); }
void UK2Node_VaRestSioCall::AddInputPin()
{
	if (!IsVariadic() || ArgumentCount >= 32) return;
	const FScopedTransaction Transaction(NSLOCTEXT("VaRestSioCall", "AddArgument", "Add Socket.IO argument"));
	Modify(); ++ArgumentCount; ReconstructNode(); FBlueprintEditorUtils::MarkBlueprintAsModified(GetBlueprint());
}
bool UK2Node_VaRestSioCall::CanRemovePin(const UEdGraphPin* Pin) const
{ return IsVariadic() && ArgumentCount > 0 && Pin && Pin->GetOwningNodeUnchecked() == this && Pin->Direction == EGPD_Input && Pin->PinName == ArgumentPinName(ArgumentCount - 1); }
void UK2Node_VaRestSioCall::RemoveInputPin(UEdGraphPin* Pin)
{
	if (!CanRemovePin(Pin)) return;
	const FScopedTransaction Transaction(NSLOCTEXT("VaRestSioCall", "RemoveArgument", "Remove Socket.IO argument"));
	Modify(); Pin->BreakAllPinLinks(true); RemovePin(Pin); --ArgumentCount; ReconstructNode(); FBlueprintEditorUtils::MarkBlueprintAsModified(GetBlueprint());
}
void UK2Node_VaRestSioCall::GetNodeContextMenuActions(UToolMenu* Menu, UGraphNodeContextMenuContext* Context) const
{
	Super::GetNodeContextMenuActions(Menu, Context);
	if (!Context || Context->bIsDebugging || !CanRemovePin(Context->Pin)) return;
	auto& Section = Menu->AddSection(TEXT("VaRestSioArguments"), NSLOCTEXT("VaRestSioCall", "ArgumentMenu", "Socket.IO Arguments"));
	Section.AddMenuEntry(TEXT("RemoveArgument"), NSLOCTEXT("VaRestSioCall", "RemoveArgument", "Remove Socket.IO argument"), FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateUObject(const_cast<UK2Node_VaRestSioCall*>(this), &UK2Node_VaRestSioCall::RemoveInputPin, const_cast<UEdGraphPin*>(Context->Pin))));
}
