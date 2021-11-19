// Alan Verbilo 2021 for VaRest plugin.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "VaRestRequestJSON.h"
#include "VaRestSubsystem.h"
#include "VaRestTypes.h"
#include "VaRestRequestAsync.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FResponseDelegate, FString, ResposeMessage);

/**
* This is async function that encapsulate callback delegates and make your code more clear.
* 
* Also this function works with String format messages instead of VaRest internal json serializer,
* to give user ability to use third party json serializer.
*
* Alan Verbilo recommend to use next free json serializer:
* https://www.unrealengine.com/marketplace/en-US/product/dbjson
*/

UCLASS()
class VAREST_API UVaRestRequestAsync : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

	UPROPERTY()
	UVaRestSubsystem* VaRestSubsystem;
	
	UPROPERTY()
	UVaRestRequestJSON* JSONRequest;

	UPROPERTY()
	UVaRestJsonObject* JsonObject;


	FString URL;
	EVaRestRequestVerb Verb;
	EVaRestRequestContentType ContentType;
	FString RequestMessage;
	
public:
	UPROPERTY(BlueprintAssignable)
	FResponseDelegate OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FResponseDelegate OnFail;

	UFUNCTION(BlueprintCallable, meta = (BlueprintUnternalUseOnly = "true", DisplayName = "VaRest Simple Request Async"))
	static UVaRestRequestAsync* VaRestRequestAsync(FString URL, EVaRestRequestVerb Verb, EVaRestRequestContentType ContentType, FString RequestMessage);

	virtual void Activate() override;

	UFUNCTION()
	void OnSuccessResend(class UVaRestRequestJSON* RequestJSON);

	UFUNCTION()
	void OnFailureResend(class UVaRestRequestJSON* RequestJSON);
};
