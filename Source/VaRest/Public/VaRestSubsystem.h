// Copyright 2014-2020 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestJsonObject.h"
#include "VaRestJsonValue.h"
#include "VaRestRequestJSON.h"

#include "Subsystems/EngineSubsystem.h"

#include "VaRestSubsystem.generated.h"

DECLARE_DYNAMIC_DELEGATE_OneParam(FVaRestCallDelegate, UVaRestRequestJSON*, Request);

USTRUCT()
struct FVaRestCallResponse
{
	GENERATED_BODY()

	UPROPERTY()
	UVaRestRequestJSON* Request;

	UPROPERTY()
	FVaRestCallDelegate Callback;

	FDelegateHandle CompleteDelegateHandle;
	FDelegateHandle FailDelegateHandle;

	FVaRestCallResponse()
		: Request(nullptr)
	{
	}
};

UCLASS()
class VAREST_API UVaRestSubsystem : public UEngineSubsystem
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
	void CallURL(const FString& URL, EVaRestRequestVerb Verb, EVaRestRequestContentType ContentType, UVaRestJsonObject* VaRestJson, const FVaRestCallDelegate& Callback);

	/** Called when URL is processed (one for both success/unsuccess events)*/
	void OnCallComplete(UVaRestRequestJSON* Request);

protected:
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
	UVaRestRequestJSON* ConstructVaRestRequestExt(EVaRestRequestVerb Verb, EVaRestRequestContentType ContentType);

	/** Create new Json object */
	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Construct Json Object"), Category = "VaRest|Subsystem")
	UVaRestJsonObject* ConstructVaRestJsonObject();

	/** Create new Json object (static one for MakeJson node, hack for #293) */
	UFUNCTION()
	static UVaRestJsonObject* StaticConstructVaRestJsonObject();

	/** Create new Json Number value
	 * Attn.!! float used instead of double to make the function blueprintable! */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Construct Json Number Value"), Category = "VaRest|Subsystem")
	UVaRestJsonValue* ConstructJsonValueNumber(float Number);

	/** Create new Json String value */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Construct Json String Value"), Category = "VaRest|Subsystem")
	UVaRestJsonValue* ConstructJsonValueString(const FString& StringValue);

	/** Create new Json Bool value */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Construct Json Bool Value"), Category = "VaRest|Subsystem")
	UVaRestJsonValue* ConstructJsonValueBool(bool InValue);

	/** Create new Json Array value */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Construct Json Array Value"), Category = "VaRest|Subsystem")
	UVaRestJsonValue* ConstructJsonValueArray(const TArray<UVaRestJsonValue*>& InArray);

	/** Create new Json Object value */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Construct Json Object Value"), Category = "VaRest|Subsystem")
	UVaRestJsonValue* ConstructJsonValueObject(UVaRestJsonObject* JsonObject);

	/** Create new Json value from FJsonValue (to be used from VaRestJsonObject) */
	UVaRestJsonValue* ConstructJsonValue(const TSharedPtr<FJsonValue>& InValue);

	//////////////////////////////////////////////////////////////////////////
	// Serialization

public:
	/** Construct Json value from string */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Subsystem")
	UVaRestJsonValue* DecodeJsonValue(const FString& JsonString);

	/** Construct Json object from string */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Subsystem")
	UVaRestJsonObject* DecodeJsonObject(const FString& JsonString);

	//////////////////////////////////////////////////////////////////////////
	// File system integration

public:
	/**
	 * Load JSON from formatted text file
	 * @param    bIsRelativeToContentDir    if set to 'false' path is treated as absolute
	 */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility")
	UVaRestJsonObject* LoadJsonFromFile(const FString& Path, const bool bIsRelativeToContentDir = true);
};
