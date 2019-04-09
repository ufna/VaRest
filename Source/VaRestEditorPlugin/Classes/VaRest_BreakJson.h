// Copyright 2015 Vladimir Alyamkin. All Rights Reserved.
// Original code by https://github.com/unktomi

#pragma once

#include "Runtime/Launch/Resources/Version.h"

#if ENGINE_MINOR_VERSION >= 15
#include "CoreMinimal.h"
#else
#include "Engine.h"
#endif
#include "K2Node.h"

#include "VaRest_BreakJson.generated.h"

UENUM(BlueprintType)
enum class EVaRest_JsonType : uint8
{
	//JSON_Null UMETA(DisplayName = "Null"),
	JSON_Bool UMETA(DisplayName = "Boolean"),
	JSON_Number UMETA(DisplayName = "Number"),
	JSON_String UMETA(DisplayName = "String"),
	JSON_Object UMETA(DisplayName = "Object")
};

USTRUCT(BlueprintType)
struct FVaRest_NamedType
{
	GENERATED_USTRUCT_BODY();

	UPROPERTY(EditAnywhere, Category = NamedType)
	FString Name;

	UPROPERTY(EditAnywhere, Category = NamedType)
	EVaRest_JsonType Type;

	UPROPERTY(EditAnywhere, Category = NamedType)
	bool bIsArray;
};

UCLASS(BlueprintType, Blueprintable)
class VARESTEDITORPLUGIN_API UVaRest_MakeJson : public UK2Node
{
	GENERATED_UCLASS_BODY()

public:
	// Begin UEdGraphNode interface.
	virtual void AllocateDefaultPins() override;
	virtual FLinearColor GetNodeTitleColor() const override;
	virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
	// End UEdGraphNode interface.

	// Begin UK2Node interface
	virtual bool IsNodePure() const { return true; }
	virtual bool ShouldShowNodeProperties() const { return true; }
	void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FText GetMenuCategory() const override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual class FNodeHandlingFunctor* CreateNodeHandler(class FKismetCompilerContext& CompilerContext) const override;
	// End UK2Node interface.

protected:
	virtual void CreateProjectionPins(UEdGraphPin* Source);

public:
	UPROPERTY(EditAnywhere, Category = PinOptions)
	TArray<FVaRest_NamedType> Inputs;
};

UCLASS(BlueprintType, Blueprintable)
class VARESTEDITORPLUGIN_API UVaRest_BreakJson : public UK2Node
{
	GENERATED_UCLASS_BODY()

public:
	// Begin UEdGraphNode interface.
	virtual void AllocateDefaultPins() override;
	virtual FLinearColor GetNodeTitleColor() const override;
	virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
	// End UEdGraphNode interface.

	// Begin UK2Node interface
	virtual bool IsNodePure() const { return true; }
	virtual bool ShouldShowNodeProperties() const { return true; }
	void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FText GetMenuCategory() const override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual class FNodeHandlingFunctor* CreateNodeHandler(class FKismetCompilerContext& CompilerContext) const override;
	// End UK2Node interface.

protected:
	virtual void CreateProjectionPins(UEdGraphPin* Source);

public:
	UPROPERTY(EditAnywhere, Category = PinOptions)
	TArray<FVaRest_NamedType> Outputs;
};
