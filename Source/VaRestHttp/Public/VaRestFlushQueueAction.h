// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "VaRestFlushQueueAction.generated.h"

class UVaRestApiClient;
namespace VaRest::Http { struct FFlushActionState; enum class EFlushDelivery : uint8; }

UENUM(BlueprintType)
enum class EVaRestFlushQueueOutcome : uint8 { Flushed, Failed, Refused };
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestFlushQueueCompleted,EVaRestFlushQueueOutcome,Outcome);

/** Flushes admitted persistence. Queue replay and network completion are separate from this operation. */
UCLASS(BlueprintType,meta=(ExposedAsyncProxy="Action"))
class VARESTHTTP_API UVaRestFlushQueueAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UVaRestFlushQueueAction(const FObjectInitializer& Initializer=FObjectInitializer::Get());
	UVaRestFlushQueueAction(FVTableHelper& Helper);
	virtual ~UVaRestFlushQueueAction() override;
	UFUNCTION(BlueprintCallable,Category="VaRest|Offline",meta=(BlueprintInternalUseOnly="true",WorldContext="WorldContextObject",DisplayName="Flush Offline Queue"))
	static UVaRestFlushQueueAction* FlushQueue(const UObject* WorldContextObject,UVaRestApiClient* Client,FString& Error);
	UFUNCTION(BlueprintPure,Category="VaRest|Offline") bool WasAdmitted() const;
	UFUNCTION(BlueprintPure,Category="VaRest|Offline") bool HasResult() const;
	UFUNCTION(BlueprintPure,Category="VaRest|Offline") bool IsFinished() const;
	UFUNCTION(BlueprintPure,Category="VaRest|Offline") EVaRestFlushQueueOutcome GetResult() const;
	UPROPERTY(BlueprintAssignable,Category="VaRest|Offline") FVaRestFlushQueueCompleted OnCompleted;
	virtual void Activate() override;
	virtual void BeginDestroy() override;
private:
	bool CanDeliverOnGT() const;
	void FinishOnGT(VaRest::Http::EFlushDelivery Result);
	void ReleaseOnGT();
	UFUNCTION() void HubClosing();
	TUniquePtr<VaRest::Http::FFlushActionState> State;
};
