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
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Send Request, Get Json, or Call Endpoint.", BlueprintInternalUseOnly="true"))
	void CallURL(const FString& URL, EVaRestRequestVerb Verb, EVaRestRequestContentType ContentType, UVaRestJsonObject* VaRestJson, const FVaRestCallDelegate& Callback);

	/** Called when URL is processed (one for both success/unsuccess events)*/
	void OnCallComplete(UVaRestRequestJSON* Request);

private:
	friend class UVaRestRequestJSON;
	void RetireCallURLSilently(UVaRestRequestJSON* Request);
	bool bCallURLClosing = false;

protected:
	UPROPERTY()
	TMap<UVaRestRequestJSON*, FVaRestCallResponse> RequestMap;

	//////////////////////////////////////////////////////////////////////////
	// Construction helpers

public:
	/** Creates new request (totally empty) */
	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Construct Json Request (Empty)", DeprecatedFunction, DeprecationMessage="Use v2 Send Request; use Hub.GetDefaultClient for shared client state.", BlueprintInternalUseOnly="true"), Category = "VaRest|Subsystem")
	UVaRestRequestJSON* ConstructVaRestRequest();

	/** Creates new request with defined verb and content type */
	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Construct Json Request", DeprecatedFunction, DeprecationMessage="Use v2 Send Request with Verb and Body.", BlueprintInternalUseOnly="true"), Category = "VaRest|Subsystem")
	UVaRestRequestJSON* ConstructVaRestRequestExt(EVaRestRequestVerb Verb, EVaRestRequestContentType ContentType);

	/** Create new Json object */
	UFUNCTION(BlueprintCallable, meta = (DisplayName = "Construct Json Object", DeprecatedFunction, DeprecationMessage="Use v2 Make Json Object.", BlueprintInternalUseOnly="true"), Category = "VaRest|Subsystem")
	UVaRestJsonObject* ConstructVaRestJsonObject();

	/** Create new Json object (static one for MakeJson node, hack for #293) */
	UFUNCTION(meta=(DeprecatedFunction, DeprecationMessage="Use v2 Make Json.", BlueprintInternalUseOnly="true"))
	static UVaRestJsonObject* StaticConstructVaRestJsonObject();

	/** Create new Json Number value
	 * Attn.!! float used instead of double to make the function blueprintable! */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Construct Json Number Value", DeprecatedFunction, DeprecationMessage="Use v2 Json From Number.", BlueprintInternalUseOnly="true"), Category = "VaRest|Subsystem")
	UVaRestJsonValue* ConstructJsonValueNumber(float Number);

	/** Create new Json String value */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Construct Json String Value", DeprecatedFunction, DeprecationMessage="Use v2 Json From String.", BlueprintInternalUseOnly="true"), Category = "VaRest|Subsystem")
	UVaRestJsonValue* ConstructJsonValueString(const FString& StringValue);

	/** Create new Json Bool value */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Construct Json Bool Value", DeprecatedFunction, DeprecationMessage="Use v2 Json From Bool.", BlueprintInternalUseOnly="true"), Category = "VaRest|Subsystem")
	UVaRestJsonValue* ConstructJsonValueBool(bool InValue);

	/** Create new Json Array value */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Construct Json Array Value", DeprecatedFunction, DeprecationMessage="Use v2 Make Json Array and Array Add.", BlueprintInternalUseOnly="true"), Category = "VaRest|Subsystem")
	UVaRestJsonValue* ConstructJsonValueArray(const TArray<UVaRestJsonValue*>& InArray);

	/** Create new Json Object value */
	UFUNCTION(BlueprintPure, meta = (DisplayName = "Construct Json Object Value", DeprecatedFunction, DeprecationMessage="Use v2 use the VaRest Json object handle directly.", BlueprintInternalUseOnly="true"), Category = "VaRest|Subsystem")
	UVaRestJsonValue* ConstructJsonValueObject(UVaRestJsonObject* JsonObject);

	/** Create new Json value from FJsonValue (to be used from VaRestJsonObject) */
	UVaRestJsonValue* ConstructJsonValue(const TSharedPtr<FJsonValue>& InValue);

	//////////////////////////////////////////////////////////////////////////
	// Serialization

public:
	/** Construct Json value from string */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Subsystem", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Parse Json.", BlueprintInternalUseOnly="true"))
	UVaRestJsonValue* DecodeJsonValue(const FString& JsonString);

	/** Construct Json object from string */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Subsystem", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Parse Json, then Get Json Type.", BlueprintInternalUseOnly="true"))
	UVaRestJsonObject* DecodeJsonObject(const FString& JsonString);

	//////////////////////////////////////////////////////////////////////////
	// File system integration

public:
	/**
	 * Load JSON from formatted text file
	 * @param    bIsRelativeToContentDir    if set to 'false' path is treated as absolute
	 */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Utility", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Load Json File (VaRest Json).", BlueprintInternalUseOnly="true"))
	UVaRestJsonObject* LoadJsonFromFile(const FString& Path, const bool bIsRelativeToContentDir = true);
};
