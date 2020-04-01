// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestDefines.h"

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

	/** Get the root Json value */
	TSharedPtr<FJsonValue>& GetRootValue();

	/** Set the root Json value */
	void SetRootValue(TSharedPtr<FJsonValue>& JsonValue);

	//////////////////////////////////////////////////////////////////////////
	// FJsonValue API

	/** Get type of Json value (Enum) */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json")
	EVaJson GetType() const;

	/** Get type of Json value (String) */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json")
	FString GetTypeString() const;

	/** Returns true if this value is a 'null' */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json")
	bool IsNull() const;

	/** Returns this value as a double, throwing an error if this is not an Json Number
	 * Attn.!! float used instead of double to make the function blueprintable! */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json")
	float AsNumber() const;

	/** Returns this value as a number, throwing an error if this is not an Json String */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json")
	FString AsString() const;

	/** Returns this value as a boolean, throwing an error if this is not an Json Bool */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json")
	bool AsBool() const;

	/** Returns this value as an array, throwing an error if this is not an Json Array */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json")
	TArray<UVaRestJsonValue*> AsArray() const;

	/** Returns this value as an object, throwing an error if this is not an Json Object */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json")
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
