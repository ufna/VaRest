// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "AssetTypeActions_Base.h"

class FVaRestApiAssetTypeActions final : public FAssetTypeActions_Base
{
public:
	explicit FVaRestApiAssetTypeActions(uint32 InCategory)
		: Category(InCategory)
	{
	}
	virtual FText GetName() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual FColor GetTypeColor() const override;
	virtual uint32 GetCategories() override;
	virtual const FSlateBrush* GetThumbnailBrush(const FAssetData& Asset, FName ClassName) const override;
	virtual const FSlateBrush* GetIconBrush(const FAssetData& Asset, FName ClassName) const override;

private:
	uint32 Category;
};
