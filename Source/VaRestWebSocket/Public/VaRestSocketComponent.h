// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "VaRestSocketClient.h"
#include "VaRestSocketComponent.generated.h"

class UVaRestApi;

UENUM(BlueprintType)
enum class EVaRestSocketScope : uint8 { Actor, GameInstance };

/** Asset entries always share their Hub socket. An Actor-scoped URL socket ends with this component. */
UCLASS(ClassGroup=(VaRest), BlueprintType, meta=(BlueprintSpawnableComponent))
class VARESTWEBSOCKET_API UVaRestSocketComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="VaRest|Socket") TObjectPtr<UVaRestApi> Api;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="VaRest|Socket") FName Entry;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="VaRest|Socket") FString Url;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="VaRest|Socket") EVaRestSocketScope Scope = EVaRestSocketScope::Actor;
	/** Required for a URL socket with GameInstance scope. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="VaRest|Socket") FName SocketName;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="VaRest|Socket") bool bAutoConnect = true;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="VaRest|Socket") bool bCloseOnEndPlay = false;
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") UVaRestSocketClient* GetClient() const { return Client; }
	UPROPERTY(BlueprintAssignable, Category="VaRestSocketTransport") FVaRestSocketConnectedEvent OnConnected;
	UPROPERTY(BlueprintAssignable, Category="VaRestSocketTransport") FVaRestSocketReadyEvent OnReady;
	UPROPERTY(BlueprintAssignable, Category="VaRestSocketTransport") FVaRestSocketDisconnectedEvent OnDisconnected;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket") FVaRestSocketRetryEvent OnRetryScheduled;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket") FVaRestSocketGaveUpEvent OnGaveUp;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket") FVaRestSocketStateEvent OnStateChanged;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket") FVaRestSocketErrorEvent OnError;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
protected:
	/** Protocol components override this accessor while reusing the lifetime and delegate bridge. */
	virtual UVaRestSocketClient* CreateClientOnGT(FVaRestError& Error);
private:
	UFUNCTION() void ForwardConnected(const FVaRestSocketOpen& Open);
	UFUNCTION() void ForwardReady(const FVaRestSocketOpen& Open, bool bIsReconnect);
	UFUNCTION() void ForwardDisconnected(const FVaRestSocketClose& Close);
	UFUNCTION() void ForwardRetry(const FVaRestSocketStatus& Status, const FVaRestSocketClose& Close);
	UFUNCTION() void ForwardGaveUp(const FVaRestError& Error, const FVaRestSocketClose& Close);
	UFUNCTION() void ForwardState(EVaRestSocketState OldState, EVaRestSocketState NewState);
	UFUNCTION() void ForwardError(const FVaRestError& Error);
	UPROPERTY(Transient) TObjectPtr<UVaRestSocketClient> Client;
	bool bActorOwned = false, bEndingPlay = false;
};
