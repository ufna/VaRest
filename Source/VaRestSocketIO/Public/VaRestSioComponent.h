// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "VaRestSocketComponent.h"
#include "VaRestSioClient.h"
#include "VaRestSioComponent.generated.h"

UCLASS(ClassGroup=(VaRest), BlueprintType, HideCategories=(VaRestSocketTransport), HideFunctions=(GetClient), meta=(BlueprintSpawnableComponent, DisplayName="VaRest Socket.IO"))
class VARESTSOCKETIO_API UVaRestSioComponent : public UVaRestSocketComponent
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="VaRest|Socket.IO") FString Namespace = TEXT("/");
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="VaRest|Socket.IO") TArray<FString> Namespaces;
	UFUNCTION(BlueprintPure, Category="VaRest|Socket.IO", meta=(DisplayName="Get Client")) UVaRestSioClient* GetSioClient() const { return SioClient; }
	UVaRestSioClient* GetClient() const { return SioClient; }
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket.IO") FVaRestSioConnectEvent OnConnect;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket.IO") FVaRestSioConnectErrorEvent OnConnectError;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket.IO") FVaRestSioDisconnectEvent OnDisconnect;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket.IO") FVaRestSioEventEvent OnEvent;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
protected:
	virtual UVaRestSocketClient* CreateClientOnGT(FVaRestError& Error) override;
private:
	UFUNCTION() void ForwardConnect(const FVaRestSioConnect& Info);
	UFUNCTION() void ForwardConnectError(const FVaRestError& Error, const FVaRestSioConnectError& Info);
	UFUNCTION() void ForwardDisconnect(const FVaRestSioDisconnect& Info);
	UFUNCTION() void ForwardEvent(const FVaRestSioEvent& Event);
	UPROPERTY(Transient) TObjectPtr<UVaRestSioClient> SioClient;
};
