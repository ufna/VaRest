// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "VaRestInspectorExport.h"
#include "Containers/Ticker.h"
#include "VaRestInspectorExportAction.generated.h"

class UVaRestHub;
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestInspectorExportDone, const FString&, Path);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestInspectorExportFailed, const FString&, Message);

UCLASS()
class VARESTINSPECTOR_API UVaRestInspectorExportAction final : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category="VaRest|Inspector", meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Export Inspector HAR"))
	static UVaRestInspectorExportAction* ExportInspectorHAR(UObject* WorldContextObject, const FString& Path, bool bAllScopes = false);
	UPROPERTY(BlueprintAssignable, Category="VaRest|Inspector") FVaRestInspectorExportDone OnDone;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Inspector") FVaRestInspectorExportFailed OnFailed;
	void Activate() override;
	void BeginDestroy() override;
	/** Module lifecycle only: retire all affine registrations on GT before joining native file jobs. */
	static void ShutdownAllOnGT();
private:
	bool Poll(float);
	void Finish(bool Success, FString Message);
	void Retire();
	TWeakObjectPtr<UObject> Context;
	TWeakObjectPtr<UVaRestHub> Hub;
	FVaRestInspectorExportTask Task;
	FTSTicker::FDelegateHandle Ticker;
	FString Destination;
	uint64 Registration = 0;
	bool bAll = false, bActivated = false, bFinished = false;
};
