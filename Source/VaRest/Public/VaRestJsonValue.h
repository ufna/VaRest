// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestJson.h"
#include "VaRestJsonValue.generated.h"

class UVaRestJsonObject;
class FJsonValue;

/**
 * Represents all the types a Json Value can be.
 */
UENUM(BlueprintType)
enum class EVaJson : uint8
{
	None,
	Null,
	String,
	Number,
	Boolean,
	Array,
	Object,
};

/**
 * Blueprintable FJsonValue wrapper
 */
UCLASS(BlueprintType, Blueprintable)
class VAREST_API UVaRestJsonValue : public UObject
{
	GENERATED_UCLASS_BODY()

public:
	/** Reset all internal data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 clear a VaRest Json variable for an invalid handle.", BlueprintInternalUseOnly="true"))
	void Reset();

	/** Get the root Json value */
	TSharedPtr<FJsonValue>& GetRootValue();

	/** Set the root Json value */
	void SetRootValue(TSharedPtr<FJsonValue>& JsonValue);
	/** Copy the legacy value into an independent v2 document; legacy nested sharing stays unchanged. */
	UFUNCTION(BlueprintCallable,BlueprintPure=false,Category="VaRest|Migration",meta=(DisplayName="To VaRest Json"))
	FVaRestJson ToVaRestJson() const;
	UFUNCTION(BlueprintCallable,BlueprintPure=false,Category="VaRest|Migration",meta=(DisplayName="To Legacy Json Value"))
	static UVaRestJsonValue* FromVaRestJson(const FVaRestJson& Json);

	//////////////////////////////////////////////////////////////////////////
	// FJsonValue API

	/** Get type of Json value (Enum) */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get Json Type.", BlueprintInternalUseOnly="true"))
	EVaJson GetType() const;

	/** Get type of Json value (String) */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get Json Type, then enum-to-string.", BlueprintInternalUseOnly="true"))
	FString GetTypeString() const;

	/** Returns true if this value is a 'null' */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Is Null (Json).", BlueprintInternalUseOnly="true"))
	bool IsNull() const;

	/** Returns this value as a double, throwing an error if this is not an Json Number
	 * Attn.!! float used instead of double to make the function blueprintable! */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 As Number (Json).", BlueprintInternalUseOnly="true"))
	float AsNumber() const;

	/** Returns this value as a int32, throwing an error if this is not an Json Number */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 As Int (Json).", BlueprintInternalUseOnly="true"))
	int32 AsInt32() const;

	/** Returns this value as a int64, throwing an error if this is not an Json Number */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 As Int64 (Json).", BlueprintInternalUseOnly="true"))
	int64 AsInt64() const;

	/** Returns this value as a string, throwing an error if this is not an Json String */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 As String (Json).", BlueprintInternalUseOnly="true"))
	FString AsString() const;

	/** Returns this value as a boolean, throwing an error if this is not an Json Bool */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 As Bool (Json).", BlueprintInternalUseOnly="true"))
	bool AsBool() const;

	/** Returns this value as an array, throwing an error if this is not an Json Array */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Array Elements (Json), or Array Get.", BlueprintInternalUseOnly="true"))
	TArray<UVaRestJsonValue*> AsArray() const;

	/** Returns this value as an object, throwing an error if this is not an Json Object */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 use the VaRest Json handle directly; check Get Json Type.", BlueprintInternalUseOnly="true"))
	UVaRestJsonObject* AsObject();

	//////////////////////////////////////////////////////////////////////////
	// Data

private:
	/** Internal JSON data */
	TSharedPtr<FJsonValue> JsonVal;

	//////////////////////////////////////////////////////////////////////////
	// Helpers

protected:
	/** Simple error logger */
	void ErrorMessage(const FString& InType) const;
};
