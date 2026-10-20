// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "UObject/Package.h"

namespace VaRest::Json::BlueprintTest
{
	/** A transient Blueprint of UObject (Kismet2.cpp:1080-1081 allows it) with an empty user function "Run" */
	struct FTestBlueprint
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		UK2Node_FunctionEntry* Entry = nullptr;
	};

	inline FTestBlueprint MakeBlueprint(const TCHAR* BaseName)
	{
		FTestBlueprint Result;
		const FName Name = MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), FName(BaseName));
		Result.Blueprint = FKismetEditorUtilities::CreateBlueprint(UObject::StaticClass(), GetTransientPackage(), Name, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (!Result.Blueprint)
		{
			return Result;
		}
		Result.Graph = FBlueprintEditorUtils::CreateNewGraph(Result.Blueprint, FName(TEXT("Run")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Result.Blueprint, Result.Graph, true, nullptr);
		TArray<UK2Node_FunctionEntry*> FunctionEntries;
		Result.Graph->GetNodesOfClass(FunctionEntries);
		Result.Entry = FunctionEntries.Num() > 0 ? FunctionEntries[0] : nullptr;
		return Result;
	}

	inline UK2Node_CallFunction* AddCall(UEdGraph* Graph, UClass* Class, FName FunctionName)
	{
		FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
		UK2Node_CallFunction* Node = Creator.CreateNode(false);
		Node->SetFromFunction(Class->FindFunctionByName(FunctionName));
		Creator.Finalize();
		return Node;
	}

	/** A Set node for a member variable; add the variable first (its pins come from the skeleton class) */
	inline UK2Node_VariableSet* AddVariableSet(UEdGraph* Graph, FName VariableName)
	{
		FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
		UK2Node_VariableSet* Node = Creator.CreateNode(false);
		Node->VariableReference.SetSelfMember(VariableName);
		Creator.Finalize();
		return Node;
	}

	inline bool AddVariable(UBlueprint* Blueprint, FName VariableName, FName Category)
	{
		return FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName,
			FEdGraphPinType(Category, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType()));
	}

	/** TryCreateConnection notifies both nodes, so wildcard pins take the linked type (EdGraphSchema.cpp:518-524) */
	inline bool Link(UEdGraphPin* From, UEdGraphPin* To)
	{
		return From && To && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(From, To);
	}

	/** Compiles with a silent log: messages stay in Results instead of reaching LogBlueprint (CompilerResultsLog.cpp:448) */
	inline bool Compile(UBlueprint* Blueprint, FCompilerResultsLog& Results)
	{
		Results.SetSilentMode(true);
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
		return Results.NumErrors == 0 && Blueprint->Status != BS_Error;
	}

	inline FString Messages(const FCompilerResultsLog& Results)
	{
		FString Text;
		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			Text += Message->ToText().ToString() + TEXT("\n");
		}
		return Text;
	}

	/** Calls the Blueprint's "Run" on a new instance and returns the instance */
	inline UObject* Run(UBlueprint* Blueprint)
	{
		UObject* Instance = NewObject<UObject>(GetTransientPackage(), Blueprint->GeneratedClass);
		UFunction* Function = Instance->FindFunction(FName(TEXT("Run")));
		if (!Function)
		{
			return nullptr;
		}
		Instance->ProcessEvent(Function, nullptr);
		return Instance;
	}
}

#endif
