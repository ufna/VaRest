// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "K2Node_CallFunction.h"
#include "VaRestSocketEndpoint.h"
#include "K2Node_VaRestSocketCall.generated.h"

class UVaRestApi;

UENUM()
enum class EVaRestSocketCall : uint8 { Get, Emit, Listen, ListenRequest, Request, Reply };

/** Persistent asset/entry/channel/route identity; generated pins are never the identity. */
UCLASS()
class UK2Node_VaRestSocketCall final : public UK2Node_CallFunction
{
	GENERATED_BODY()
public:
	UK2Node_VaRestSocketCall();
	UPROPERTY(EditAnywhere, Category="Socket") TObjectPtr<UVaRestApi> Api;
	UPROPERTY(EditAnywhere, Category="Socket") FName Entry;
	UPROPERTY(VisibleAnywhere, Category="Message") FString Channel;
	UPROPERTY(VisibleAnywhere, Category="Message") FString Route;
	UPROPERTY() EVaRestSocketCall Operation = EVaRestSocketCall::Get;
	UPROPERTY() bool bAssetEntry = false;
	UPROPERTY() bool bDeclaration = false;
	const UVaRestSocketEndpoint* ResolveEntry() const;
	const FVaRestSocketMessageDeclaration* ResolveDeclaration() const;
	virtual void AllocateDefaultPins() override;
	virtual void ReallocatePinsDuringReconstruction(TArray<UEdGraphPin*>& OldPins) override;
	virtual void NotifyPinConnectionListChanged(UEdGraphPin* Pin) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
	virtual void ValidateNodeDuringCompilation(FCompilerResultsLog& Log) const override;
	virtual void ExpandNode(FKismetCompilerContext& Compiler, UEdGraph* Graph) override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& Registrar) const override;
	virtual FText GetNodeTitle(ENodeTitleType::Type Type) const override;
	virtual FText GetTooltipText() const override;
	virtual FText GetMenuCategory() const override;
private:
	FString PinError;
};
