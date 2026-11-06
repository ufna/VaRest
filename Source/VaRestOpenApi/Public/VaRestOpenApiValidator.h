// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "EditorValidatorBase.h"
#include "VaRestOpenApiValidator.generated.h"

UCLASS()
class VARESTOPENAPI_API UVaRestOpenApiValidator : public UEditorValidatorBase
{
	GENERATED_BODY()
protected:
	virtual bool CanValidateAsset_Implementation(const FAssetData& AssetData,UObject* Object,FDataValidationContext& Context) const override;
	virtual EDataValidationResult ValidateLoadedAsset_Implementation(const FAssetData& AssetData,UObject* Object,FDataValidationContext& Context) override;
};
