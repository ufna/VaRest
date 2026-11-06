// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "K2Node_BaseAsyncTask.h"
#include "K2Node_VaRestSocketCall.h"
#include "K2Node_VaRestSocketAsync.generated.h"

/** Raw JSON/message actions. Reflected worker output stays a separate shared-carrier capability. */
UCLASS()
class UK2Node_VaRestSocketAsync final : public UK2Node_BaseAsyncTask
{
	GENERATED_BODY()
public:
	UK2Node_VaRestSocketAsync();
	UPROPERTY(EditAnywhere, Category="Socket") TObjectPtr<UVaRestApi> Api;
	UPROPERTY(EditAnywhere, Category="Socket") FName Entry;
	UPROPERTY(VisibleAnywhere, Category="Message") FString Channel;
	UPROPERTY(VisibleAnywhere, Category="Message") FString Route;
	UPROPERTY() EVaRestSocketCall Operation = EVaRestSocketCall::Listen;
	UPROPERTY() bool bAssetEntry = false;
	UPROPERTY() bool bDeclaration = false;
	UPROPERTY(EditAnywhere, Category="Payload", meta=(EditCondition="!bDeclaration")) FVaRestTypeRef PayloadType;
	UPROPERTY(EditAnywhere, Category="Reply", meta=(EditCondition="!bDeclaration")) FVaRestTypeRef ReplyType;
	const UVaRestSocketEndpoint* ResolveEntry() const;
	const FVaRestSocketMessageDeclaration* ResolveDeclaration() const;
	virtual void AllocateDefaultPins() override;
	virtual void ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
	virtual void ValidateNodeDuringCompilation(FCompilerResultsLog& Log) const override;
	virtual void ExpandNode(FKismetCompilerContext& Compiler, UEdGraph* Graph) override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& Registrar) const override;
	virtual FText GetNodeTitle(ENodeTitleType::Type Type) const override;
	virtual FText GetTooltipText() const override;
	virtual FText GetMenuCategory() const override;
protected:
	virtual bool HandleDelegates(const TArray<FBaseAsyncTaskHelper::FOutputPinAndLocalVariable>& VariableOutputs,
		UEdGraphPin* ProxyObjectPin, UEdGraphPin*& InOutLastThenPin, UEdGraph* SourceGraph, FKismetCompilerContext& CompilerContext) override;
private:
	FString PinError;
};
