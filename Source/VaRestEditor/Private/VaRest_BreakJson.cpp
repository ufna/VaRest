// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.
// Original code by https://github.com/unktomi

#include "VaRest_BreakJson.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNodeUtils.h" // for FNodeTextCache
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "EditorCategoryUtils.h"
#include "KismetCompiler.h"
#include "Runtime/Launch/Resources/Version.h"

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
		UEdGraphPin* InputPin = nullptr;

		for (int32 PinIndex = 0; PinIndex < Node->Pins.Num(); ++PinIndex)
		{
			UEdGraphPin* Pin = Node->Pins[PinIndex];
			if (Pin && (EGPD_Input == Pin->Direction))
			{
				InputPin = Pin;
				break;
			}
		}

		UEdGraphPin* InNet = FEdGraphUtilities::GetNetFromPin(InputPin);
		UClass* Class = Cast<UClass>(StaticLoadObject(UClass::StaticClass(), nullptr, TEXT("class'VaRest.VaRestJsonObject'")));

		FBPTerminal** SourceTerm = Context.NetMap.Find(InNet);
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

				FBPTerminal** Target = Context.NetMap.Find(Pin);

				const FName& FieldName = Pin->PinName;
				const FName& FieldType = Pin->PinType.PinCategory;

				FBPTerminal* FieldNameTerm = Context.CreateLocalTerminal(ETerminalSpecification::TS_Literal);
				FieldNameTerm->Type.PinCategory = CompilerContext.GetSchema()->PC_String;
				FieldNameTerm->SourcePin = Pin;

				FieldNameTerm->Name = FieldName.ToString();
				FieldNameTerm->TextLiteral = FText::FromName(FieldName);

				FBlueprintCompiledStatement& Statement = Context.AppendStatementForNode(Node);
				FName FunctionName;

				const bool bIsArray = Pin->PinType.ContainerType == EPinContainerType::Array;
				if (FieldType == CompilerContext.GetSchema()->PC_Boolean)
				{
					FunctionName = bIsArray ? TEXT("GetBoolArrayField") : TEXT("GetBoolField");
				}
				else if (FieldType == CompilerContext.GetSchema()->PC_Real)
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

				UFunction* FunctionPtr = Class->FindFunctionByName(FunctionName);
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
		UEdGraphPin* InputPin = nullptr;
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
		FBPTerminal** TermPtr = Context.NetMap.Find(Net);

		if (!TermPtr)
		{
			FBPTerminal* Term = Context.CreateLocalTerminalFromPinAutoChooseScope(Net, Context.NetNameMap->MakeValidName(Net));

			Context.NetMap.Add(Net, Term);

			return Term;
		}

		return *TermPtr;
	}

	void RegisterOutputTerm(FKismetFunctionContext& Context, UEdGraphPin* OutputPin, FBPTerminal* ContextTerm)
	{
		FBPTerminal* Term = Context.CreateLocalTerminalFromPinAutoChooseScope(OutputPin, Context.NetNameMap->MakeValidName(OutputPin));
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
				if (nullptr != Pin && EGPD_Output == Pin->Direction)
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
UVaRest_BreakJson::UVaRest_BreakJson(const FObjectInitializer& ObjectInitializer)
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

	UClass* Class = Cast<UClass>(StaticLoadObject(UClass::StaticClass(), nullptr, TEXT("class'VaRest.VaRestJsonObject'")));
	UEdGraphPin* Pin = CreatePin(EGPD_Input, K2Schema->PC_Object, TEXT(""), Class, TEXT("Target"));

	K2Schema->SetPinAutogeneratedDefaultValueBasedOnType(Pin);

	CreateProjectionPins(Pin);
}

FLinearColor UVaRest_BreakJson::GetNodeTitleColor() const
{
	return FLinearColor(255.0f, 255.0f, 0.0f);
}

void UVaRest_BreakJson::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = (PropertyChangedEvent.Property != nullptr) ? PropertyChangedEvent.Property->GetFName() : NAME_None;
	if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UVaRest_BreakJson, Outputs) ||
		PropertyName == GET_MEMBER_NAME_STRING_CHECKED(FVaRest_NamedType, Name) ||
		PropertyName == GET_MEMBER_NAME_STRING_CHECKED(FVaRest_NamedType, Type) ||
		PropertyName == GET_MEMBER_NAME_STRING_CHECKED(FVaRest_NamedType, bIsArray))
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
		CachedCategory.SetCachedText(FEditorCategoryUtils::BuildCategoryString(FCommonEditorCategory::Utilities, LOCTEXT("ActionMenuCategory", "VaRest")), this);
	}
	return CachedCategory;
}

void UVaRest_BreakJson::CreateProjectionPins(UEdGraphPin* Source)
{
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	UClass* Class = Cast<UClass>(StaticLoadObject(UClass::StaticClass(), nullptr, TEXT("class'VaRest.VaRestJsonObject'")));

	for (TArray<FVaRest_NamedType>::TIterator it(Outputs); it; ++it)
	{
		FName Type;
		FName SubCategory;

		UObject* Subtype = nullptr;

		switch ((*it).Type)
		{
		case EVaRest_JsonType::JSON_Bool:
			Type = K2Schema->PC_Boolean;
			break;

		case EVaRest_JsonType::JSON_Number:
			Type = K2Schema->PC_Real;
			SubCategory = K2Schema->PC_Double;
			break;

		case EVaRest_JsonType::JSON_String:
			Type = K2Schema->PC_String;
			break;

		case EVaRest_JsonType::JSON_Object:
			Type = K2Schema->PC_Object;
			Subtype = Class;
			break;
		}

		UEdGraphNode::FCreatePinParams OutputPinParams;
		OutputPinParams.ContainerType = (*it).bIsArray ? EPinContainerType::Array : EPinContainerType::None;
		UEdGraphPin* OutputPin = CreatePin(EGPD_Output, Type, SubCategory, Subtype, (*it).Name, OutputPinParams);
	}
}

FText UVaRest_BreakJson::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("VaRest_Break_Json.NodeTitle", "Break Json");
}

class FKCHandler_MakeJson : public FNodeHandlingFunctor
{

public:
	FKCHandler_MakeJson(FKismetCompilerContext& InCompilerContext)
		: FNodeHandlingFunctor(InCompilerContext)
	{
	}

	virtual void Compile(FKismetFunctionContext& Context, UEdGraphNode* Node) override
	{
		UEdGraphPin* OutputPin = nullptr;

		for (int32 PinIndex = 0; PinIndex < Node->Pins.Num(); ++PinIndex)
		{
			UEdGraphPin* Pin = Node->Pins[PinIndex];
			if (Pin && (EGPD_Output == Pin->Direction))
			{
				OutputPin = Pin;
				break;
			}
		}

		UClass* Class = Cast<UClass>(StaticLoadObject(UClass::StaticClass(), nullptr, TEXT("class'VaRest.VaRestJsonObject'")));

		FBPTerminal** TargetTerm = Context.NetMap.Find(OutputPin);
		if (TargetTerm == nullptr)
		{
			return;
		}

		{
			UClass* SubsystemClass = Cast<UClass>(StaticLoadObject(UClass::StaticClass(), nullptr, TEXT("class'VaRest.VaRestSubsystem'")));

			const FName FunctionName = TEXT("StaticConstructVaRestJsonObject");
			UFunction* FunctionPtr = SubsystemClass->FindFunctionByName(FunctionName);
			FBlueprintCompiledStatement& Statement = Context.AppendStatementForNode(Node);
			Statement.Type = KCST_CallFunction;
			Statement.FunctionToCall = FunctionPtr;
			Statement.FunctionContext = nullptr;
			Statement.bIsParentContext = false;
			Statement.LHS = *TargetTerm;
			FBPTerminal* NullTerm = Context.CreateLocalTerminal(ETerminalSpecification::TS_Literal);
			NullTerm->Type.PinCategory = CompilerContext.GetSchema()->PC_Object;
			NullTerm->ObjectLiteral = nullptr;
			NullTerm->SourcePin = OutputPin;
			Statement.RHS.Add(NullTerm);
		}

		for (int32 PinIndex = 0; PinIndex < Node->Pins.Num(); ++PinIndex)
		{
			UEdGraphPin* Pin = Node->Pins[PinIndex];
			if (Pin && (EGPD_Input == Pin->Direction))
			{
				FBPTerminal** Source = Context.NetMap.Find(FEdGraphUtilities::GetNetFromPin(Pin));

				const FName& FieldName = Pin->PinName;
				const FName& FieldType = Pin->PinType.PinCategory;

				FBPTerminal* FieldNameTerm = Context.CreateLocalTerminal(ETerminalSpecification::TS_Literal);
				FieldNameTerm->Type.PinCategory = CompilerContext.GetSchema()->PC_String;
				FieldNameTerm->SourcePin = Pin;

				FieldNameTerm->Name = FieldName.ToString();
				FieldNameTerm->TextLiteral = FText::FromName(FieldName);

				FBlueprintCompiledStatement& Statement = Context.AppendStatementForNode(Node);
				FName FunctionName;

				const bool bIsArray = Pin->PinType.ContainerType == EPinContainerType::Array;
				if (FieldType == CompilerContext.GetSchema()->PC_Boolean)
				{
					FunctionName = bIsArray ? TEXT("SetBoolArrayField") : TEXT("SetBoolField");
				}
				else if (FieldType == CompilerContext.GetSchema()->PC_Real)
				{
					FunctionName = bIsArray ? TEXT("SetNumberArrayFieldDouble") : TEXT("SetNumberFieldDouble");
				}
				else if (FieldType == CompilerContext.GetSchema()->PC_String)
				{
					FunctionName = bIsArray ? TEXT("SetStringArrayField") : TEXT("SetStringField");
				}
				else if (FieldType == CompilerContext.GetSchema()->PC_Object)
				{
					FunctionName = bIsArray ? TEXT("SetObjectArrayField") : TEXT("SetObjectField");
				}
				else
				{
					continue;
				}

				UFunction* FunctionPtr = Class->FindFunctionByName(FunctionName);
				Statement.Type = KCST_CallFunction;
				Statement.FunctionToCall = FunctionPtr;
				Statement.FunctionContext = *TargetTerm;
				Statement.bIsParentContext = false;
				Statement.LHS = nullptr;
				Statement.RHS.Add(FieldNameTerm);
				Statement.RHS.Add(*Source);
			}
		}
	}

	FBPTerminal* RegisterInputTerm(FKismetFunctionContext& Context, UEdGraphPin* InputPin)
	{
		// Find structure source net
		UEdGraphPin* Net = FEdGraphUtilities::GetNetFromPin(InputPin);
		FBPTerminal** TermPtr = Context.NetMap.Find(Net);

		if (!TermPtr)
		{
			FBPTerminal* Term = Context.CreateLocalTerminalFromPinAutoChooseScope(Net, Context.NetNameMap->MakeValidName(Net));

			Context.NetMap.Add(Net, Term);

			return Term;
		}

		return *TermPtr;
	}

	void RegisterOutputTerm(FKismetFunctionContext& Context, UEdGraphPin* OutputPin)
	{
		FBPTerminal* Term = Context.CreateLocalTerminalFromPinAutoChooseScope(OutputPin, Context.NetNameMap->MakeValidName(OutputPin));
		Context.NetMap.Add(OutputPin, Term);
	}

	virtual void RegisterNets(FKismetFunctionContext& Context, UEdGraphNode* InNode) override
	{
		UVaRest_MakeJson* Node = Cast<UVaRest_MakeJson>(InNode);
		FNodeHandlingFunctor::RegisterNets(Context, Node);

		check(NULL != Node);
		{
			for (int32 PinIndex = 0; PinIndex < Node->Pins.Num(); ++PinIndex)
			{
				UEdGraphPin* Pin = Node->Pins[PinIndex];
				if (EGPD_Output == Pin->Direction)
				{
					RegisterOutputTerm(Context, Pin);
				}
				else
				{
					RegisterInputTerm(Context, Pin);
				}
			}
		}
	}
};

/**
 * Main node class
 */
UVaRest_MakeJson::UVaRest_MakeJson(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

FNodeHandlingFunctor* UVaRest_MakeJson::CreateNodeHandler(class FKismetCompilerContext& CompilerContext) const
{
	return new FKCHandler_MakeJson(CompilerContext);
}

void UVaRest_MakeJson::AllocateDefaultPins()
{
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

	UClass* Class = Cast<UClass>(StaticLoadObject(UClass::StaticClass(), nullptr, TEXT("class'VaRest.VaRestJsonObject'")));
	UEdGraphPin* Pin = CreatePin(EGPD_Output, K2Schema->PC_Object, TEXT(""), Class, TEXT("Target"));

	K2Schema->SetPinAutogeneratedDefaultValueBasedOnType(Pin);

	CreateProjectionPins(Pin);
}

FLinearColor UVaRest_MakeJson::GetNodeTitleColor() const
{
	return FLinearColor(255.0f, 255.0f, 0.0f);
}

void UVaRest_MakeJson::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = (PropertyChangedEvent.Property != nullptr) ? PropertyChangedEvent.Property->GetFName() : NAME_None;
	if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UVaRest_MakeJson, Inputs) ||
		PropertyName == GET_MEMBER_NAME_STRING_CHECKED(FVaRest_NamedType, Name) ||
		PropertyName == GET_MEMBER_NAME_STRING_CHECKED(FVaRest_NamedType, Type) ||
		PropertyName == GET_MEMBER_NAME_STRING_CHECKED(FVaRest_NamedType, bIsArray))
	{
		ReconstructNode();
		GetGraph()->NotifyGraphChanged();
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}

void UVaRest_MakeJson::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
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

FText UVaRest_MakeJson::GetMenuCategory() const
{
	static FNodeTextCache CachedCategory;

	if (CachedCategory.IsOutOfDate(this))
	{
		// FText::Format() is slow, so we cache this to save on performance
		CachedCategory.SetCachedText(FEditorCategoryUtils::BuildCategoryString(FCommonEditorCategory::Utilities, LOCTEXT("ActionMenuCategory", "VaRest")), this);
	}
	return CachedCategory;
}

void UVaRest_MakeJson::CreateProjectionPins(UEdGraphPin* Source)
{
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	UClass* Class = Cast<UClass>(StaticLoadObject(UClass::StaticClass(), nullptr, TEXT("class'VaRest.VaRestJsonObject'")));

	for (TArray<FVaRest_NamedType>::TIterator it(Inputs); it; ++it)
	{
		FName Type;
		FName SubCategory;
		UObject* Subtype = nullptr;

		switch ((*it).Type)
		{
		case EVaRest_JsonType::JSON_Bool:
			Type = K2Schema->PC_Boolean;
			break;

		case EVaRest_JsonType::JSON_Number:
			Type = K2Schema->PC_Real;
			SubCategory = K2Schema->PC_Double;
			break;

		case EVaRest_JsonType::JSON_String:
			Type = K2Schema->PC_String;
			break;

		case EVaRest_JsonType::JSON_Object:
			Type = K2Schema->PC_Object;
			Subtype = Class;
			break;
		}

		UEdGraphNode::FCreatePinParams InputPinParams;
		InputPinParams.ContainerType = (*it).bIsArray ? EPinContainerType::Array : EPinContainerType::None;
		UEdGraphPin* InputPin = CreatePin(EGPD_Input, Type, SubCategory, Subtype, (*it).Name, InputPinParams);

		InputPin->SetSavePinIfOrphaned(false);
	}
}

FText UVaRest_MakeJson::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("VaRest_Make_Json.NodeTitle", "Make Json");
}

#undef LOCTEXT_NAMESPACE
