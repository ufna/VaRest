// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "Kismet/BlueprintAsyncActionBase.h"
#include "VaRestDynamicResult.h"
#include "VaRestPreparedOutputAction.generated.h"

class UVaRestPreparedOutputAction;
class FVaRestPreparedValue;
struct FVaRestPreparedOutputState;

DECLARE_DYNAMIC_DELEGATE_TwoParams(FVaRestProtocolPrepare, UVaRestPreparedOutputAction*, Action, const FString&, RequestIdText);
DECLARE_DYNAMIC_DELEGATE_FourParams(FVaRestProtocolContinue, UVaRestPreparedOutputAction*, Action, const FString&, RequestIdText, bool, bSuccess, const FVaRestError&, Error);
DECLARE_DYNAMIC_DELEGATE_ThreeParams(FVaRestProtocolUnavailable, UVaRestPreparedOutputAction*, Action, const FString&, RequestIdText, const FVaRestError&, OriginalError);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestProtocolOutputUnavailable, const FVaRestError&, Error);

/** Shared GTK-only protocol adapter. No reflected value is stored across trace or user execution. */
UCLASS(Abstract)
class VARESTHTTP_API UVaRestPreparedOutputAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UVaRestPreparedOutputAction(const FObjectInitializer& Initializer = FObjectInitializer::Get());
	UVaRestPreparedOutputAction(FVTableHelper& Helper);
	virtual ~UVaRestPreparedOutputAction() override;
	UFUNCTION(BlueprintCallable, Category="VaRest|Internal", meta=(BlueprintInternalUseOnly="true"))
	bool ConfigureOutputType(const FVaRestTypeRef& Type);
	UFUNCTION(BlueprintCallable, Category="VaRest|Internal", meta=(BlueprintInternalUseOnly="true"))
	bool BindOutput(FVaRestProtocolPrepare Prepare, FVaRestProtocolContinue Continue, FVaRestProtocolUnavailable Unavailable);
	UFUNCTION(BlueprintCallable, CustomThunk, Category="VaRest|Internal", meta=(BlueprintInternalUseOnly="true", CustomStructureParam="Value"))
	bool TakeResponse(int32& Value);
	DECLARE_FUNCTION(execTakeResponse);
	UFUNCTION(BlueprintCallable, Category="VaRest|Internal", meta=(BlueprintInternalUseOnly="true"))
	bool ConfirmPrepared(const FString& RequestIdText);
	UFUNCTION(BlueprintCallable, Category="VaRest|Internal", meta=(BlueprintInternalUseOnly="true"))
	bool AcceptPreparedOutput(const FString& StoredCookie, const FString& RequestIdText);
	UFUNCTION(BlueprintPure, Category="VaRest|Internal", meta=(BlueprintInternalUseOnly="true"))
	static TArray<FVaRestJsonError> GetPreparedDecodeWarnings(UVaRestPreparedOutputAction* Action, const FString& RequestIdText);
	UPROPERTY(BlueprintAssignable, Category="VaRest") FVaRestProtocolOutputUnavailable OnTypedOutputUnavailable;
	virtual void BeginDestroy() override;
protected:
	bool AdmitPreparedOutputOnGT(const UObject* Owner, const UObject* RuleOwner,
		TSharedPtr<FVaRestDynamicBudget, ESPMode::ThreadSafe> Budget, const FVaRestJsonConvertOptions& Options, FVaRestError& Error);
	bool HasPreparedOutputOnGT() const;
	void SetPositionalOutputOnGT(bool bPositional);
	/** Runs only the compiler-generated closed preparation. Call before a logical terminal trace. */
	FString PrepareOutputOnGT(const FVaRestJson& Value, const FVaRestJsonConvertOptions& Options,
		TFunction<bool()> Current, FVaRestError& Error);
	/** Revalidates after metadata/trace callbacks. Retires only this cookie, including on nested deliveries. */
	bool DeliverPreparedOutputOnGT(const FString& Cookie);
	void RetirePreparedOutputOnGT();
	virtual bool CanDeliverPreparedOutputOnGT() const { return false; }
private:
	bool TargetCurrentOnGT() const;
	bool TakeOnGT(const FProperty* Property, void* Address);
	void UnavailableOnGT(const FString& Cookie, FVaRestError Error);
	UPROPERTY(Transient) FVaRestTypeRef OutputType;
	UPROPERTY(Transient) FVaRestProtocolPrepare PrepareDelegate;
	UPROPERTY(Transient) FVaRestProtocolContinue ContinueDelegate;
	UPROPERTY(Transient) FVaRestProtocolUnavailable UnavailableDelegate;
	TSharedPtr<FVaRestPreparedOutputState> PreparedState;
};
