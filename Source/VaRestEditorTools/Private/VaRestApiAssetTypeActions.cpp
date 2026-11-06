// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestApiAssetTypeActions.h"

#include "Styling/AppStyle.h"
#include "VaRestApi.h"

#define LOCTEXT_NAMESPACE "VaRestApiAssetActions"

FText FVaRestApiAssetTypeActions::GetName() const
{
	return LOCTEXT("DisplayName", "VaRest API");
}
UClass* FVaRestApiAssetTypeActions::GetSupportedClass() const
{
	return UVaRestApi::StaticClass();
}
FColor FVaRestApiAssetTypeActions::GetTypeColor() const
{
	return FColor(0, 147, 203);
}
uint32 FVaRestApiAssetTypeActions::GetCategories()
{
	return Category;
}
const FSlateBrush* FVaRestApiAssetTypeActions::GetThumbnailBrush(const FAssetData&, FName) const
{
	return FAppStyle::GetBrush(TEXT("ClassThumbnail.DataAsset"));
}
const FSlateBrush* FVaRestApiAssetTypeActions::GetIconBrush(const FAssetData&, FName) const
{
	return FAppStyle::GetBrush(TEXT("ClassIcon.DataAsset"));
}

#undef LOCTEXT_NAMESPACE
