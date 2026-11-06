// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "K2Node_VaRestSendRequest.h"
#include "K2Node_VaRestCallEndpoint.generated.h"

UCLASS()
class UK2Node_VaRestCallEndpoint final : public UK2Node_VaRestRequestBase
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere,Category="Endpoint") TObjectPtr<UVaRestApi> Api;
	UPROPERTY(VisibleAnywhere,Category="Endpoint") FGuid EndpointId;
	/** Legacy/name selection fallback only when no GUID was selected; a rename retains the GUID. */
	UPROPERTY(EditAnywhere,Category="Endpoint",meta=(GetOptions="GetEndpointNames")) FName EndpointName;
	UFUNCTION() TArray<FString> GetEndpointNames() const;
	virtual UVaRestApi* GetApi() const override { return Api; }
	virtual bool IsEndpointNode() const override { return true; }
	virtual const FVaRestApiEndpoint* ResolveEndpoint() const override;
	virtual FVaRestTypeRef GetResponseType() const override;
	virtual FVaRestTypeRef GetErrorType() const override;
	virtual bool AllowsQueue() const override;
	virtual FText GetNodeTitle(ENodeTitleType::Type Type) const override;
	virtual FText GetTooltipText() const override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& Registrar) const override;
};
