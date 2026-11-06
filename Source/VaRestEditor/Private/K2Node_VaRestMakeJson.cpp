// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "K2Node_VaRestMakeJson.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Framework/Commands/UIAction.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "KismetCompiler.h"
#include "ScopedTransaction.h"
#include "Textures/SlateIcon.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "VaRestJson.h"
#include "VaRestJsonLibrary.h"

#define LOCTEXT_NAMESPACE "K2Node_VaRestMakeJson"

namespace
{
bool RefusesJsonType(const FEdGraphPinType& Type, FString& OutReason)
{
	// Mirrors the converter's property support, including the stricter JSON object-key types.
	const auto IsNotData = [](FName Category)
	{
		return Category == UEdGraphSchema_K2::PC_Exec || Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class
			|| Category == UEdGraphSchema_K2::PC_Interface || Category == UEdGraphSchema_K2::PC_Delegate || Category == UEdGraphSchema_K2::PC_MCDelegate
			|| Category == UEdGraphSchema_K2::PC_FieldPath;
	};
	if (IsNotData(Type.PinCategory) || (Type.IsMap() && IsNotData(Type.PinValueType.TerminalCategory)))
	{
		OutReason = LOCTEXT("NotJson", "Make Json values must be data: objects, classes, interfaces, delegates, field paths and exec are not converted to JSON").ToString();
		return true;
	}
	if (Type.IsMap() && Type.PinCategory != UEdGraphSchema_K2::PC_String && Type.PinCategory != UEdGraphSchema_K2::PC_Name
		&& Type.PinCategory != UEdGraphSchema_K2::PC_Int && Type.PinCategory != UEdGraphSchema_K2::PC_Int64
		&& Type.PinCategory != UEdGraphSchema_K2::PC_Byte && Type.PinCategory != UEdGraphSchema_K2::PC_Enum
		&& Type.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
	{
		OutReason = LOCTEXT("NotJsonMapKey", "Map keys must be strings, names, integers or enums (they become JSON object keys)").ToString();
		return true;
	}
	return false;
}
}

const FName UK2Node_VaRestMakeJson::JsonPinName(TEXT("Json"));

FName UK2Node_VaRestMakeJson::PinNameFor(int32 Id)
{
	return FName(*FString::Printf(TEXT("Value_%d"), Id));
}

void UK2Node_VaRestMakeJson::AssignEntryIds()
{
	TSet<int32> Used;
	for (FVaRestMakeJsonEntry& Entry : Entries)
	{
		bool bAlreadyUsed = false;
		Used.Add(Entry.Id, &bAlreadyUsed);
		if (Entry.Id <= 0 || bAlreadyUsed)
		{
			Entry.Id = NextId++;
			Used.Add(Entry.Id);
		}
		NextId = FMath::Max(NextId, Entry.Id + 1);
	}
}

void UK2Node_VaRestMakeJson::CreateValuePin(const FVaRestMakeJsonEntry& Entry)
{
	UEdGraphPin* ValuePin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Wildcard, PinNameFor(Entry.Id));
	ValuePin->PinFriendlyName = Entry.Key.IsEmpty() ? LOCTEXT("EmptyKey", "(empty key)") : FText::FromString(Entry.Key);
}

void UK2Node_VaRestMakeJson::AllocateDefaultPins()
{
	AssignEntryIds();
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Struct, FVaRestJson::StaticStruct(), JsonPinName);
	for (const FVaRestMakeJsonEntry& Entry : Entries)
	{
		CreateValuePin(Entry);
	}
	Super::AllocateDefaultPins();
}

FText UK2Node_VaRestMakeJson::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("Title", "Make Json");
}

FText UK2Node_VaRestMakeJson::GetTooltipText() const
{
	return LOCTEXT("Tooltip", "VaRest 2: a JSON object from named values. Each value pin takes whatever is connected (number, string, struct, array, map, Json); structs use camelCase field names. Keys are case-sensitive; edit them in the Details panel.\nPure, like Make Array: every node that reads the output gets its own new object. To add fields with Set Field, use Set Field's return value (or Make Json Object) - a second reader of this pin would not see them.");
}

FText UK2Node_VaRestMakeJson::GetKeywords() const
{
	// The palette has two "Make Json" entries: VaRest 1's (Utilities|VaRest) and this one; search by "VaRest 2" finds this
	return LOCTEXT("Keywords", "VaRest 2 v2 Json object key value construct");
}

UEdGraphPin* UK2Node_VaRestMakeJson::GetJsonPin() const
{
	return FindPinChecked(JsonPinName, EGPD_Output);
}

UEdGraphPin* UK2Node_VaRestMakeJson::GetValuePin(int32 Index) const
{
	return Entries.IsValidIndex(Index) ? FindPin(PinNameFor(Entries[Index].Id), EGPD_Input) : nullptr;
}

int32 UK2Node_VaRestMakeJson::IndexOfValuePin(const UEdGraphPin* Pin) const
{
	if (!Pin || Pin->Direction != EGPD_Input || Pin->GetOwningNodeUnchecked() != this)
	{
		return INDEX_NONE;
	}
	return Entries.IndexOfByPredicate([Pin](const FVaRestMakeJsonEntry& Entry) { return PinNameFor(Entry.Id) == Pin->PinName; });
}

void UK2Node_VaRestMakeJson::SyncValuePinType(UEdGraphPin* ValuePin)
{
	if (ValuePin->LinkedTo.Num() > 0 && ValuePin->LinkedTo[0]->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
	{
		ValuePin->PinType = ValuePin->LinkedTo[0]->PinType; // what FCustomStructureParamHelper does (K2Node_CallFunction.cpp:132)
		return;
	}
	if (ValuePin->LinkedTo.Num() == 0)
	{
		ValuePin->PinType = FEdGraphPinType();
		ValuePin->PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
	}
}

void UK2Node_VaRestMakeJson::ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins)
{
	Super::ReallocatePinsDuringReconstruction(OldPins); // AllocateDefaultPins + RestoreSplitPins (K2Node.cpp:646-651)
	// New value pins start as wildcards; give each the type its old pin had, so the by-name rewiring that follows
	// (K2Node.cpp:758) reconnects pins of the same type
	for (const UEdGraphPin* OldPin : OldPins)
	{
		if (OldPin->Direction == EGPD_Input && OldPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
		{
			if (UEdGraphPin* NewPin = FindPin(OldPin->PinName, EGPD_Input))
			{
				NewPin->PinType = OldPin->PinType;
			}
		}
	}
}

void UK2Node_VaRestMakeJson::PostReconstructNode()
{
	Super::PostReconstructNode();
	for (UEdGraphPin* NodePin : Pins)
	{
		if (IndexOfValuePin(NodePin) != INDEX_NONE)
		{
			SyncValuePinType(NodePin);
		}
	}
}

void UK2Node_VaRestMakeJson::NotifyPinConnectionListChanged(UEdGraphPin* Pin)
{
	Super::NotifyPinConnectionListChanged(Pin);
	if (IndexOfValuePin(Pin) != INDEX_NONE)
	{
		SyncValuePinType(Pin);
		GetGraph()->NotifyNodeChanged(this);
	}
}

bool UK2Node_VaRestMakeJson::IsConnectionDisallowed(const UEdGraphPin* MyPin, const UEdGraphPin* OtherPin, FString& OutReason) const
{
	if (IndexOfValuePin(MyPin) != INDEX_NONE)
	{
		if (RefusesJsonType(OtherPin->PinType, OutReason)) return true;
	}
	return Super::IsConnectionDisallowed(MyPin, OtherPin, OutReason);
}

void UK2Node_VaRestMakeJson::ReportEntryErrors(FCompilerResultsLog& MessageLog) const
{
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		// The key follows the format's only "@@", and Tokenize keeps text after the last argument's "@@" as plain text
		// (CompilerResultsLog.h:347-372), so a key containing "@@" is printed as written
		const FString& Key = Entries[Index].Key;
		if (Key.IsEmpty())
		{
			MessageLog.Error(*LOCTEXT("EmptyKeyError", "@@ has a value with an empty key").ToString(), this);
		}
		// Case-sensitive on purpose: TSet<FString> would treat "id" and "ID" as one key
		for (int32 Earlier = 0; Earlier < Index; ++Earlier)
		{
			if (!Key.IsEmpty() && Entries[Earlier].Key.Equals(Key, ESearchCase::CaseSensitive))
			{
				MessageLog.Error(*FString::Printf(TEXT("@@: the key \"%s\" appears twice"), *Key), this);
				break;
			}
		}
		const UEdGraphPin* ValuePin = GetValuePin(Index);
		if (!ValuePin || ValuePin->LinkedTo.Num() == 0 || ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
		{
			MessageLog.Error(*FString::Printf(TEXT("@@: connect a value for the key \"%s\""), *Key), this);
		}
		else
		{
			FString Reason;
			if (RefusesJsonType(ValuePin->PinType, Reason))
				MessageLog.Error(*FString::Printf(TEXT("@@: the value for key \"%s\" is unsupported: %s"), *Key, *Reason), this);
		}
	}
}

void UK2Node_VaRestMakeJson::ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const
{
	Super::ValidateNodeDuringCompilation(MessageLog);
	// An expanded node has handed its value links to the intermediate nodes and reported its errors in ExpandNode
	if (!bExpanded)
	{
		ReportEntryErrors(MessageLog);
	}
}

void UK2Node_VaRestMakeJson::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	Super::ExpandNode(CompilerContext, SourceGraph);
	// Report while the links are still here: the compiler validates this node only after the expansion (see bExpanded)
	ReportEntryErrors(CompilerContext.MessageLog);
	bExpanded = true;

	UK2Node_CallFunction* MakeObject = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	MakeObject->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, MakeJsonInternalObject), UVaRestJsonLibrary::StaticClass());
	MakeObject->AllocateDefaultPins();
	UEdGraphPin* Current = MakeObject->GetReturnValuePin();

	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		UEdGraphPin* ValuePin = GetValuePin(Index);
		if (!ValuePin || ValuePin->LinkedTo.Num() == 0 || ValuePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
		{
			continue; // reported above; the compile fails
		}
		UK2Node_CallFunction* SetNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
		SetNode->FunctionReference.SetExternalMember(GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary, MakeJsonInternalSet), UVaRestJsonLibrary::StaticClass());
		SetNode->AllocateDefaultPins();
		Current->MakeLinkTo(SetNode->FindPinChecked(FName(TEXT("Json"))));
		SetNode->FindPinChecked(FName(TEXT("Key")))->DefaultValue = Entries[Index].Key;
		UEdGraphPin* SetValuePin = SetNode->FindPinChecked(FName(TEXT("Value")));
		// The CustomStructureParam pin takes our type before the links move (K2Node_GetDataTableRow.cpp:405-407)
		SetValuePin->PinType = ValuePin->PinType;
		CompilerContext.MovePinLinksToIntermediate(*ValuePin, *SetValuePin);
		Current = SetNode->GetReturnValuePin();
	}
	CompilerContext.MovePinLinksToIntermediate(*GetJsonPin(), *Current);
	BreakAllNodeLinks();
}

void UK2Node_VaRestMakeJson::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	// Registered under the node class, as v1's Make Json does (VaRest_BreakJson.cpp:483-502)
	UClass* ActionKey = GetClass();
	if (ActionRegistrar.IsOpenForRegistration(ActionKey))
	{
		UBlueprintNodeSpawner* NodeSpawner = UBlueprintNodeSpawner::Create(GetClass());
		check(NodeSpawner != nullptr);
		ActionRegistrar.AddBlueprintAction(ActionKey, NodeSpawner);
	}
}

FText UK2Node_VaRestMakeJson::GetMenuCategory() const
{
	return LOCTEXT("MenuCategory", "VaRest|Json");
}

void UK2Node_VaRestMakeJson::AddInputPin()
{
	Modify();
	FVaRestMakeJsonEntry Entry;
	Entry.Id = NextId++;
	Entry.Key = TEXT("key");
	for (int32 Suffix = 2; Entries.ContainsByPredicate([&Entry](const FVaRestMakeJsonEntry& Other) { return Other.Key.Equals(Entry.Key, ESearchCase::CaseSensitive); }); ++Suffix)
	{
		Entry.Key = FString::Printf(TEXT("key%d"), Suffix);
	}
	Entries.Add(Entry);
	CreateValuePin(Entry);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(GetBlueprint());
}

bool UK2Node_VaRestMakeJson::CanRemovePin(const UEdGraphPin* Pin) const
{
	return IndexOfValuePin(Pin) != INDEX_NONE;
}

void UK2Node_VaRestMakeJson::RemoveInputPin(UEdGraphPin* Pin)
{
	const int32 Index = IndexOfValuePin(Pin);
	if (Index == INDEX_NONE)
	{
		return;
	}
	const FScopedTransaction Transaction(LOCTEXT("RemoveKeyTransaction", "Remove Make Json Key"));
	Modify();
	Pin->BreakAllPinLinks(true);
	Entries.RemoveAt(Index);
	RemovePin(Pin); // EdGraphNode.cpp:464-485
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(GetBlueprint());
}

void UK2Node_VaRestMakeJson::GetNodeContextMenuActions(UToolMenu* Menu, UGraphNodeContextMenuContext* Context) const
{
	Super::GetNodeContextMenuActions(Menu, Context);
	if (Context->bIsDebugging || !Context->Pin || !CanRemovePin(Context->Pin))
	{
		return;
	}
	// As K2Node_DoOnceMultiInput.cpp:291-305 offers "Remove pin"
	FToolMenuSection& Section = Menu->AddSection(FName(TEXT("VaRestMakeJson")), LOCTEXT("MenuSection", "Make Json"));
	Section.AddMenuEntry(FName(TEXT("RemoveKey")), LOCTEXT("RemoveKey", "Remove key"), LOCTEXT("RemoveKeyTooltip", "Remove this key and its value pin"), FSlateIcon(),
		FUIAction(FExecuteAction::CreateUObject(const_cast<UK2Node_VaRestMakeJson*>(this), &UK2Node_VaRestMakeJson::RemoveInputPin, const_cast<UEdGraphPin*>(Context->Pin))));
}

void UK2Node_VaRestMakeJson::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	// A key edited in the Details panel: new pins and names (v1's Make Json does the same, VaRest_BreakJson.cpp:468-481)
	AssignEntryIds();
	ReconstructNode();
	GetGraph()->NotifyGraphChanged();
	FBlueprintEditorUtils::MarkBlueprintAsModified(GetBlueprint());
	Super::PostEditChangeProperty(PropertyChangedEvent);
}

#undef LOCTEXT_NAMESPACE
