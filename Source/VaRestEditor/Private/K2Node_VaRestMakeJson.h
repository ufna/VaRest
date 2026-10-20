// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "K2Node.h"
#include "K2Node_AddPinInterface.h"

#include "K2Node_VaRestMakeJson.generated.h"

/** One key of a Make Json node */
USTRUCT()
struct FVaRestMakeJsonEntry
{
	GENERATED_BODY()

	/** The JSON key, exactly as written: keys are case-sensitive ("id" and "ID" are two keys) */
	UPROPERTY(EditAnywhere, Category = "Make Json")
	FString Key;

	/** Names the value pin. Pins are FNames, which ignore case, so they cannot be named after keys. 0 = not assigned yet. */
	UPROPERTY()
	int32 Id = 0;
};

/**
 * VaRest 2 Make Json (spec 2.5): a JSON object from named values, each value pin a wildcard that takes the connected
 * type. Pure, like Make Array: every consumer gets its own object. Expands to MakeJsonInternalObject and one
 * MakeJsonInternalSet per key.
 */
UCLASS()
class UK2Node_VaRestMakeJson : public UK2Node, public IK2Node_AddPinInterface
{
	GENERATED_BODY()

public:
	/** The keys, in output order. Edit them in the Details panel; "+" adds one, a value pin's context menu removes it. */
	UPROPERTY(EditAnywhere, Category = "Make Json")
	TArray<FVaRestMakeJsonEntry> Entries;

	static const FName JsonPinName;

	// UEdGraphNode
	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FText GetKeywords() const override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void GetNodeContextMenuActions(UToolMenu* Menu, UGraphNodeContextMenuContext* Context) const override;
	virtual void ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const override;

	// UK2Node
	virtual bool IsNodePure() const override { return true; }
	virtual bool ShouldShowNodeProperties() const override { return true; }
	virtual void ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins) override;
	virtual void PostReconstructNode() override;
	virtual void NotifyPinConnectionListChanged(UEdGraphPin* Pin) override;
	virtual bool IsConnectionDisallowed(const UEdGraphPin* MyPin, const UEdGraphPin* OtherPin, FString& OutReason) const override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FText GetMenuCategory() const override;

	// IK2Node_AddPinInterface
	virtual void AddInputPin() override;
	virtual void RemoveInputPin(UEdGraphPin* Pin) override;
	virtual bool CanRemovePin(const UEdGraphPin* Pin) const override;

	UEdGraphPin* GetJsonPin() const;

	/** The value pin of Entries[Index], or null */
	UEdGraphPin* GetValuePin(int32 Index) const;

	/** Index into Entries of one of this node's value pins; INDEX_NONE for any other pin */
	int32 IndexOfValuePin(const UEdGraphPin* Pin) const;

private:
	UPROPERTY()
	int32 NextId = 1;

	/**
	 * Set by ExpandNode. The compiler validates a node after expanding it (KismetCompiler.cpp:4566 then :4589, :4140
	 * then :4174), when the value links have already moved to the intermediate nodes. It expands a copy of the node (graphs
	 * are cloned, :4538 and :4502), and a copy is built from the UPROPERTYs, so a node in the editor never has this set.
	 */
	bool bExpanded = false;

	static FName PinNameFor(int32 Id);

	/** Errors for empty keys, exact duplicate keys and unconnected values */
	void ReportEntryErrors(FCompilerResultsLog& MessageLog) const;

	/** Entries added or duplicated in the Details panel arrive with Id 0 or a copied Id */
	void AssignEntryIds();

	void CreateValuePin(const FVaRestMakeJsonEntry& Entry);

	/** A linked value pin has the linked type; an unlinked one is a wildcard */
	static void SyncValuePinType(UEdGraphPin* ValuePin);
};
