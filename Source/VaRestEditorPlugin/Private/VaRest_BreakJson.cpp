// Copyright 2015 Vladimir Alyamkin. All Rights Reserved.
// Original code by https://github.com/unktomi

#include "VaRestEditorPluginPrivatePCH.h"
#include "VaRest_BreakJson.h"

#define LOCTEXT_NAMESPACE "VaRest_BreakJson"

class FKCHandler_BreakJson : public FNodeHandlingFunctor
{
	
public:
	FKCHandler_BreakJson(FKismetCompilerContext& InCompilerContext)
		: FNodeHandlingFunctor(InCompilerContext)
	{
	}

	virtual void Compile(FKismetFunctionContext& Context, UEdGraphNode* Node) override
	{
		UEdGraphPin* InputPin = NULL;

		for (int32 PinIndex = 0; PinIndex < Node->Pins.Num(); ++PinIndex)
		{
			UEdGraphPin* Pin = Node->Pins[PinIndex];
			if (Pin && (EGPD_Input == Pin->Direction))
			{
				InputPin = Pin;
				break;
			}
		}

		UEdGraphPin *InNet = FEdGraphUtilities::GetNetFromPin(InputPin);
		UClass *Class = Cast<UClass>(StaticLoadObject(UClass::StaticClass(), NULL, TEXT("class'VaRestPlugin.VaRestJsonObject'")));

		FBPTerminal **SourceTerm = Context.NetMap.Find(InNet);
		if (SourceTerm == nullptr)
		{
			return;
		}

		for (int32 PinIndex = 0; PinIndex < Node->Pins.Num(); ++PinIndex)
		{
			UEdGraphPin* Pin = Node->Pins[PinIndex];
			if (Pin && (EGPD_Output == Pin->Direction))
			{
				if (Pin->LinkedTo.Num() < 1)
				{
					continue;
				}

				FBPTerminal **Target = Context.NetMap.Find(Pin);

				const FString &FieldName = Pin->PinName;
				const FString &FieldType = Pin->PinType.PinCategory;

				FBPTerminal* FieldNameTerm = Context.CreateLocalTerminal(ETerminalSpecification::TS_Literal);
				FieldNameTerm->Type.PinCategory = CompilerContext.GetSchema()->PC_String;
				FieldNameTerm->Source = Pin;
				FieldNameTerm->Name = FieldName;
				FieldNameTerm->TextLiteral = FText::FromString(FieldName);

 				FBlueprintCompiledStatement& Statement = Context.AppendStatementForNode(Node);
				FName FunctionName;

				bool bIsArray = Pin->PinType.bIsArray;

				if (FieldType == CompilerContext.GetSchema()->PC_Boolean)
				{
					FunctionName = bIsArray ? TEXT("GetBoolArrayField") : TEXT("GetBoolField");
				}
				else if (FieldType == CompilerContext.GetSchema()->PC_Float)
				{
					FunctionName = bIsArray ? TEXT("GetNumberArrayField") : TEXT("GetNumberField");
				}
				else if (FieldType == CompilerContext.GetSchema()->PC_String)
				{
					FunctionName = bIsArray ? TEXT("GetStringArrayField") : TEXT("GetStringField");
				}
				else if (FieldType == CompilerContext.GetSchema()->PC_Object)
				{
					FunctionName = bIsArray ? TEXT("GetObjectArrayField") : TEXT("GetObjectField");
				}
				else
				{
					continue;
				}
			
				UFunction *FunctionPtr = Class->FindFunctionByName(FunctionName);
				Statement.Type = KCST_CallFunction;
				Statement.FunctionToCall = FunctionPtr;
				Statement.FunctionContext = *SourceTerm;
				Statement.bIsParentContext = false;
				Statement.LHS = *Target;
				Statement.RHS.Add(FieldNameTerm);
			}
		}
	}

	FBPTerminal* RegisterInputTerm(FKismetFunctionContext& Context, UVaRest_BreakJson* Node)
	{
		// Find input pin
		UEdGraphPin* InputPin = NULL;
		for (int32 PinIndex = 0; PinIndex < Node->Pins.Num(); ++PinIndex)
		{
			UEdGraphPin* Pin = Node->Pins[PinIndex];
			if (Pin && (EGPD_Input == Pin->Direction))
			{
				InputPin = Pin;
				break;
			}
		}
		check(NULL != InputPin);

		// Find structure source net
		UEdGraphPin* Net = FEdGraphUtilities::GetNetFromPin(InputPin);
		FBPTerminal **TermPtr = Context.NetMap.Find(Net);

		if (!TermPtr)
		{
			FBPTerminal *Term = Context.CreateLocalTerminalFromPinAutoChooseScope(Net, Context.NetNameMap->MakeValidName(Net));

			Context.NetMap.Add(Net, Term);

			return Term;
		}

		return *TermPtr;
	}

	void RegisterOutputTerm(FKismetFunctionContext& Context, UEdGraphPin* OutputPin, FBPTerminal* ContextTerm)
	{
		FBPTerminal *Term = Context.CreateLocalTerminalFromPinAutoChooseScope(OutputPin, Context.NetNameMap->MakeValidName(OutputPin));
		Context.NetMap.Add(OutputPin, Term);
	}

	virtual void RegisterNets(FKismetFunctionContext& Context, UEdGraphNode* InNode) override
	{
		UVaRest_BreakJson* Node = Cast<UVaRest_BreakJson>(InNode);
		FNodeHandlingFunctor::RegisterNets(Context, Node);

		check(NULL != Node);

		if (FBPTerminal* StructContextTerm = RegisterInputTerm(Context, Node))
		{
			for (int32 PinIndex = 0; PinIndex < Node->Pins.Num(); ++PinIndex)
			{
				UEdGraphPin* Pin = Node->Pins[PinIndex];
				if (NULL != Pin && EGPD_Output == Pin->Direction)
				{
					RegisterOutputTerm(Context, Pin, StructContextTerm);
				}
			}
		}
	}
};

/**
 * Main node class
 */
UVaRest_BreakJson::UVaRest_BreakJson(const FObjectInitializer &ObjectInitializer)
	: Super(ObjectInitializer)
{
}

FNodeHandlingFunctor* UVaRest_BreakJson::CreateNodeHandler(class FKismetCompilerContext& CompilerContext) const 
{
	return new FKCHandler_BreakJson(CompilerContext);
}

void UVaRest_BreakJson::AllocateDefaultPins()
{
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

	UClass *Class = Cast<UClass>(StaticLoadObject(UClass::StaticClass(), NULL, TEXT("class'VaRestPlugin.VaRestJsonObject'")));
	UEdGraphPin* Pin = CreatePin(EGPD_Input, K2Schema->PC_Object, TEXT(""), Class, false, false, TEXT("Target"));

	K2Schema->SetPinDefaultValueBasedOnType(Pin);

	CreateProjectionPins(Pin);
}

FLinearColor UVaRest_BreakJson::GetNodeTitleColor() const
{
	return FLinearColor(255.0f, 255.0f, 0.0f);
}

void UVaRest_BreakJson::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	bool bIsDirty = false;

	FName PropertyName = (PropertyChangedEvent.Property != NULL) ? PropertyChangedEvent.Property->GetFName() : NAME_None;
	if (true || PropertyName == TEXT("Outputs"))
	{
		bIsDirty = true;
	}

	if (bIsDirty)
	{
		ReconstructNode();
		GetGraph()->NotifyGraphChanged();
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}

void UVaRest_BreakJson::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	// actions get registered under specific object-keys; the idea is that 
	// actions might have to be updated (or deleted) if their object-key is  
	// mutated (or removed)... here we use the node's class (so if the node 
	// type disappears, then the action should go with it)
	UClass* ActionKey = GetClass();

	// to keep from needlessly instantiating a UBlueprintNodeSpawner, first   
	// check to make sure that the registrar is looking for actions of this type
	// (could be regenerating actions for a specific asset, and therefore the 
	// registrar would only accept actions corresponding to that asset)
	if (ActionRegistrar.IsOpenForRegistration(ActionKey))
	{
		UBlueprintNodeSpawner* NodeSpawner = UBlueprintNodeSpawner::Create(GetClass());
		check(NodeSpawner != nullptr);

		ActionRegistrar.AddBlueprintAction(ActionKey, NodeSpawner);
	}
}

FText UVaRest_BreakJson::GetMenuCategory() const
{
	static FNodeTextCache CachedCategory;

	if (CachedCategory.IsOutOfDate(this))
	{
		// FText::Format() is slow, so we cache this to save on performance
		CachedCategory.SetCachedText(FEditorCategoryUtils::BuildCategoryString(FCommonEditorCategory::Utilities, LOCTEXT("ActionMenuCategory", "Va Rest")), this);
	}
	return CachedCategory;
}

void UVaRest_BreakJson::CreateProjectionPins(UEdGraphPin *Source)
{
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	UClass *Class = Cast<UClass>(StaticLoadObject(UClass::StaticClass(), NULL, TEXT("class'VaRestPlugin.VaRestJsonObject'")));

	for (TArray<FVaRest_NamedType>::TIterator it(Outputs); it; ++it)
	{
		FString Type;
		UObject *Subtype = nullptr;
		FString FieldName = (*it).Name;

		switch ((*it).Type) {
			case EVaRest_JsonType::JSON_Bool:
				Type = K2Schema->PC_Boolean;
				break;

			case EVaRest_JsonType::JSON_Number:
				Type = K2Schema->PC_Float;
				break;

			case EVaRest_JsonType::JSON_String:
				Type = K2Schema->PC_String;
				break;

			case EVaRest_JsonType::JSON_Object:
				Type = K2Schema->PC_Object;
				Subtype = Class;
				break;
		}

		UEdGraphPin *OutputPin = CreatePin(EGPD_Output, Type, TEXT(""), Subtype, (*it).bIsArray, false, (*it).Name);
	}
}

FText UVaRest_BreakJson::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("VaRest_Break_Json.NodeTitle", "Break Json");
}
