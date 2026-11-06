// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "K2Node_BaseAsyncTask.h"
#include "K2Node_VaRestSioCall.h"
#include "K2Node_VaRestSioAsync.generated.h"

/** Persistent declaration identity. Native JSON/Event/Ack delivery does not enable reflected worker transfers. */
UCLASS()
class UK2Node_VaRestSioAsync final : public UK2Node_BaseAsyncTask
{
	GENERATED_BODY()
public:
	UK2Node_VaRestSioAsync();
	UPROPERTY(EditAnywhere, Category="Socket.IO") TObjectPtr<UVaRestApi> Api;
	UPROPERTY(EditAnywhere, Category="Socket.IO") FName Entry;
	UPROPERTY(VisibleAnywhere, Category="Event") FString Namespace;
	UPROPERTY(VisibleAnywhere, Category="Event") FString Event;
	UPROPERTY(EditAnywhere, Category="Event") bool bOnce = false;
	UPROPERTY(EditAnywhere, Category="Payload", meta=(EditCondition="!bDeclaration")) FVaRestTypeRef PayloadType;
	UPROPERTY(EditAnywhere, Category="Reply", meta=(EditCondition="!bDeclaration")) FVaRestTypeRef ReplyType;
	UPROPERTY() EVaRestSioCall Operation = EVaRestSioCall::Listen;
	UPROPERTY() bool bAssetEntry = false;
	UPROPERTY() bool bDeclaration = false;
	const UVaRestSioEndpoint* ResolveEntry() const;
	const FVaRestSioEventDeclaration* ResolveDeclaration() const;
	virtual void AllocateDefaultPins() override;
	virtual void ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins) override;
	virtual void NotifyPinConnectionListChanged(UEdGraphPin* Pin) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Change) override;
	virtual void ValidateNodeDuringCompilation(FCompilerResultsLog& Log) const override;
	virtual void ExpandNode(FKismetCompilerContext& Compiler, UEdGraph* Graph) override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& Registrar) const override;
	virtual FText GetNodeTitle(ENodeTitleType::Type Type) const override;
	virtual FText GetTooltipText() const override;
	virtual FText GetMenuCategory() const override;
private:
	FString PinError;
protected:
	virtual bool HandleDelegates(const TArray<FBaseAsyncTaskHelper::FOutputPinAndLocalVariable>& VariableOutputs,
		UEdGraphPin* ProxyObjectPin, UEdGraphPin*& InOutLastThenPin, UEdGraph* SourceGraph, FKismetCompilerContext& CompilerContext) override;
};
