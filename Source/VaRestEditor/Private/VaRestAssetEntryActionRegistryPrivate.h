// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace VaRestEditor
{
struct FAssetEntryRegistryState;
class FAssetEntryRegistryOwner final
{
public:
	static TUniquePtr<FAssetEntryRegistryOwner> CreateOnGT();
	~FAssetEntryRegistryOwner();
private:
	TSharedPtr<FAssetEntryRegistryState> State;
};
}
