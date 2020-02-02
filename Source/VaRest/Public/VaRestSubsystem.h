// Copyright 2014-2020 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestRequestJSON.h"

#include "Delegates/DelegateCombinations.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/SubsystemCollection.h"

#include "VaRestSubsystem.generated.h"

class UVaRestRequestJSON;
class UVaRestSettings;

DECLARE_DYNAMIC_DELEGATE_OneParam(FVaRestCallDelegate, UVaRestRequestJSON*, Request);

USTRUCT()
struct FVaRestCallResponse
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY()
	UVaRestRequestJSON* Request;

	UPROPERTY()
	UObject* WorldContextObject;

	UPROPERTY()
	FVaRestCallDelegate Callback;

	FDelegateHandle CompleteDelegateHandle;
	FDelegateHandle FailDelegateHandle;

	FVaRestCallResponse()
		: Request(nullptr)
		, WorldContextObject(nullptr)
	{
	}
};

UCLASS()
class VAREST_API UVaRestSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	UVaRestSubsystem();

	// Begin USubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	// End USubsystem

	//////////////////////////////////////////////////////////////////////////
	// Easy URL processing

public:
	/** Easy way to process http requests */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility")
	void CallURL(const FString& URL, ERequestVerb Verb, ERequestContentType ContentType, UVaRestJsonObject* VaRestJson, const FVaRestCallDelegate& Callback);

	/** Called when URL is processed (one for both success/unsuccess events)*/
	void OnCallComplete(UVaRestRequestJSON* Request);

private:
	UPROPERTY()
	TMap<UVaRestRequestJSON*, FVaRestCallResponse> RequestMap;

	//////////////////////////////////////////////////////////////////////////
	// Construction helpers

public:
	/** Creates new request (totally empty) */
	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Construct Json Request (Empty)"), Category = "VaRest|Subsystem")
	UVaRestRequestJSON* ConstructVaRestRequest();

	/** Creates new request with defined verb and content type */
	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Construct Json Request"), Category = "VaRest|Subsystem")
	UVaRestRequestJSON* ConstructVaRestRequestExt(ERequestVerb Verb, ERequestContentType ContentType);

	/** Create new Json object */
	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Construct Json Object"), Category = "VaRest|Subsystem")
	UVaRestJsonObject* ConstructVaRestJsonObject();

	//////////////////////////////////////////////////////////////////////////
	// File system integration

public:
	/** 
	 * Load JSON from formatted text file
	 * @param    bIsRelativeToContentDir    if set to 'false' path is treated as absolute
	 */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility")
	class UVaRestJsonObject* LoadJsonFromFile(const FString& Path, const bool bIsRelativeToContentDir = true);

	//////////////////////////////////////////////////////////////////////////
	// Data getters and helpers

public:
	/** Getter for internal settings object to support runtime configuration changes */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Subsystem")
	UVaRestSettings* GetSettings() const;

private:
	/** Plugin settings */
	UVaRestSettings* Settings;
};
