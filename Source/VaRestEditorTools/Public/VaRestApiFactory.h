// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"

#include "VaRestApiFactory.generated.h"

/** Creates a reusable API asset; source import/reimport remains a separate factory. */
UCLASS()
class VARESTEDITORTOOLS_API UVaRestApiFactory : public UFactory
{
	GENERATED_BODY()
public:
	UVaRestApiFactory();
	virtual UObject* FactoryCreateNew(UClass* Class, UObject* Parent, FName Name, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn) override;
	virtual FText GetDisplayName() const override;
	virtual uint32 GetMenuCategories() const override;
	virtual FName GetNewAssetThumbnailOverride() const override;
	virtual FName GetNewAssetIconOverride() const override;
};
