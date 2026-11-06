// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "K2Node_CallFunction.h"
#include "K2Node_AddPinInterface.h"
#include "VaRestSioEndpoint.h"
#include "K2Node_VaRestSioCall.generated.h"

class UVaRestApi;

UENUM()
enum class EVaRestSioCall : uint8 { Get, Emit, Listen, ListenWithAck, EmitWithAck, Reply };

UCLASS()
class UK2Node_VaRestSioCall final : public UK2Node_CallFunction, public IK2Node_AddPinInterface
{
	GENERATED_BODY()
public:
	UK2Node_VaRestSioCall();
	UPROPERTY(EditAnywhere, Category="Socket.IO") TObjectPtr<UVaRestApi> Api;
	UPROPERTY(EditAnywhere, Category="Socket.IO") FName Entry;
	UPROPERTY(VisibleAnywhere, Category="Event") FString Namespace;
	UPROPERTY(VisibleAnywhere, Category="Event") FString Event;
	UPROPERTY() EVaRestSioCall Operation = EVaRestSioCall::Get;
	UPROPERTY() bool bAssetEntry = false;
	UPROPERTY() bool bDeclaration = false;
	UPROPERTY(EditAnywhere, Category="Arguments", meta=(ClampMin="0", ClampMax="32", EditCondition="Operation == EVaRestSioCall::Emit && !bDeclaration && !bAssetEntry")) int32 ArgumentCount = 1;
	const UVaRestSioEndpoint* ResolveEntry() const;
	const FVaRestSioEventDeclaration* ResolveDeclaration() const;
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
	virtual void AddInputPin() override;
	virtual bool CanAddPin() const override { return IsVariadic() && ArgumentCount < 32; }
	virtual void RemoveInputPin(UEdGraphPin* Pin) override;
	virtual bool CanRemovePin(const UEdGraphPin* Pin) const override;
	virtual void GetNodeContextMenuActions(UToolMenu* Menu, UGraphNodeContextMenuContext* Context) const override;
private:
	bool IsVariadic() const { return Operation == EVaRestSioCall::Emit && !bAssetEntry && !bDeclaration; }
	static FName ArgumentPinName(int32 Index);
	FString PinError;
};

namespace VaRestEditor::SioNodes
{
	const UVaRestSioEndpoint* ResolveEntry(const UVaRestApi* Api, FName Entry);
	const FVaRestSioEventDeclaration* ResolveDeclaration(const UVaRestSioEndpoint* Endpoint, FStringView Namespace, FStringView Event, EVaRestSioCall Operation);
	bool PayloadPinType(const FVaRestTypeRef& Type, FEdGraphPinType& PinType, FString& Error);
	void ValidateIdentity(const UK2Node& Node, const UVaRestSioEndpoint* Endpoint, bool bAssetEntry, bool bDeclaration, const FVaRestSioEventDeclaration* Declaration, FCompilerResultsLog& Log);
}
