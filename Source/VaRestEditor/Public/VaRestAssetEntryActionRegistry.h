// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BlueprintNodeSpawner.h"
#include "UObject/StrongObjectPtr.h"

class UVaRestApi;
class UVaRestApiSocketConfig;

/** Protocol modules own their concrete node identity {Asset,Entry,Channel,Route}, pins and validation. GT only. */
class VARESTEDITOR_API IVaRestAssetEntryActionProvider
{
public:
	virtual ~IVaRestAssetEntryActionProvider();
	virtual FName GetProtocolTag() const=0;
	/** Entry=null requests API-level actions (for example HTTP endpoints). Socket entries are protocol-filtered. */
	virtual void GatherActions(UVaRestApi& Api,UVaRestApiSocketConfig* Entry,TArray<UBlueprintNodeSpawner*>& Out) const=0;
	/** Reconstruct this provider's loaded affected nodes once after the asset's action database refresh. */
	virtual void RefreshLoadedNodes(UVaRestApi& Api) const=0;
};

/** Module-owned registry. Each tag has one provider; only its exact registration handle can remove it. */
struct VARESTEDITOR_API FVaRestAssetEntryActionRegistry final
{
	/** Unregister before module unload, outside provider/action-database callbacks. Busy/duplicate/empty registration is refused. */
	static FDelegateHandle RegisterProvider(TSharedRef<IVaRestAssetEntryActionProvider> Provider,FString& Error);
	static bool UnregisterProvider(FDelegateHandle Handle);
	static bool IsRegistered(FDelegateHandle Handle);
	/** Pins every returned spawner across later provider/registration callouts. Caller owns the returned pins. */
	static TArray<TStrongObjectPtr<UBlueprintNodeSpawner>> GatherActions(UVaRestApi* Api);
};
