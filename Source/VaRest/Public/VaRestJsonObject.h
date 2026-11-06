// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestDefines.h"

#include "Dom/JsonObject.h"
#include "Templates/UnrealTypeTraits.h"
#include "VaRestJson.h"

#include "VaRestJsonObject.generated.h"

class UVaRestJsonValue;

/**
 * Blueprintable FJsonObject wrapper
 */
UCLASS(BlueprintType, Blueprintable)
class VAREST_API UVaRestJsonObject : public UObject
{
	GENERATED_UCLASS_BODY()

public:
	/** Reset all internal data */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Make Json Object, then assign the returned handle.", BlueprintInternalUseOnly="true"))
	void Reset();

	/** Get the root Json object */
	TSharedRef<FJsonObject>& GetRootObject();

	/** Set the root Json object */
	void SetRootObject(const TSharedPtr<FJsonObject>& JsonObject);

	/** Copy this legacy reference graph into an independent v2 document. */
	UFUNCTION(BlueprintCallable,BlueprintPure=false,Category="VaRest|Migration",meta=(DisplayName="To VaRest Json"))
	FVaRestJson ToVaRestJson() const;
	/** Copy a v2 object into a new legacy wrapper. Returns null for a non-object value. */
	UFUNCTION(BlueprintCallable,BlueprintPure=false,Category="VaRest|Migration",meta=(DisplayName="To Legacy Json Object"))
	static UVaRestJsonObject* FromVaRestJson(const FVaRestJson& Json);

	//////////////////////////////////////////////////////////////////////////
	// Serialization

	/** Serialize Json to string (formatted with line breaks) */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 To String (Json), Pretty enabled.", BlueprintInternalUseOnly="true"))
	FString EncodeJson() const;

	/** Serialize Json to string (single string without line breaks) */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 To String (Json).", BlueprintInternalUseOnly="true"))
	FString EncodeJsonToSingleString() const;

	/** Construct Json object from string */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Parse Json.", BlueprintInternalUseOnly="true"))
	bool DecodeJson(const FString& JsonString, bool bUseIncrementalParser = true);

	//////////////////////////////////////////////////////////////////////////
	// FJsonObject API

	/** Gets the type of value as string for a given field */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get Field, then Get Json Type.", BlueprintInternalUseOnly="true"))
	FString GetFieldTypeString(const FString& FieldName) const;

	/** Returns a list of field names that exist in the object */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get Keys (Json).", BlueprintInternalUseOnly="true"))
	TArray<FString> GetFieldNames() const;

	/** Checks to see if the FieldName exists in the object */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Has Field (VaRest Json).", BlueprintInternalUseOnly="true"))
	bool HasField(const FString& FieldName) const;

	/** Remove field named FieldName */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Remove Field (VaRest Json).", BlueprintInternalUseOnly="true"))
	void RemoveField(const FString& FieldName);

	/** Get the field named FieldName as a JsonValue */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	UVaRestJsonValue* GetField(const FString& FieldName) const;

	/** Add a field named FieldName with a Value */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	void SetField(const FString& FieldName, UVaRestJsonValue* JsonValue);

	/** Get the field named FieldName as a Json Array */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get Array Field (VaRest Json).", BlueprintInternalUseOnly="true"))
	TArray<UVaRestJsonValue*> GetArrayField(const FString& FieldName) const;

	/** Set an ObjectField named FieldName and value of Json Array */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	void SetArrayField(const FString& FieldName, const TArray<UVaRestJsonValue*>& InArray);

	/** Adds all of the fields from one json object to this one */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Merge Json.", BlueprintInternalUseOnly="true"))
	void MergeJsonObject(UVaRestJsonObject* InJsonObject, bool Overwrite);

	//////////////////////////////////////////////////////////////////////////
	// FJsonObject API Helpers (easy to use with simple Json objects)

	/** Get the field named FieldName as a number. Ensures that the field is present and is of type Json number.
	 * Attn.!! float used instead of double to make the function blueprintable! */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get Number Field.", BlueprintInternalUseOnly="true"))
	float GetNumberField(const FString& FieldName) const;

	/** DEPRECATED Attn.!! float used instead of double to make the function blueprintable! */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	void SetNumberField(const FString& FieldName, float Number);

	/** Add a field named FieldName with Number as value */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	void SetNumberFieldDouble(const FString& FieldName, double Number);

	/** Get the field named FieldName as an Integer. Ensures that the field is present and is of type Json number. */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get Int Field.", BlueprintInternalUseOnly="true"))
	int32 GetIntegerField(const FString& FieldName) const;

	/** Add a field named FieldName with Integer as value. */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	void SetIntegerField(const FString& FieldName, int32 Number);

	/** Get the field named FieldName as an Int64. Ensures that the field is present and is of type Json number. */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get Int64 Field.", BlueprintInternalUseOnly="true"))
	int64 GetInt64Field(const FString& FieldName) const;

	/** Add a field named FieldName with Int64 as value. */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	void SetInt64Field(const FString& FieldName, int64 Number);

	/** Get the field named FieldName as a string. */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get String Field (VaRest Json).", BlueprintInternalUseOnly="true"))
	FString GetStringField(const FString& FieldName) const;

	/** Add a field named FieldName with value of StringValue */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	void SetStringField(const FString& FieldName, const FString& StringValue);

	/** Get the field named FieldName as a boolean. */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get Bool Field (VaRest Json).", BlueprintInternalUseOnly="true"))
	bool GetBoolField(const FString& FieldName) const;

	/** Set a boolean field named FieldName and value of InValue */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	void SetBoolField(const FString& FieldName, bool InValue);

	/** Get the field named FieldName as a Json object. */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get Object Field (VaRest Json).", BlueprintInternalUseOnly="true"))
	UVaRestJsonObject* GetObjectField(const FString& FieldName) const;

	/** Set an ObjectField named FieldName and value of JsonObject */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	void SetObjectField(const FString& FieldName, UVaRestJsonObject* JsonObject);

	/** Set a map of fields with String values */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field for each map entry, or Struct To Json for a whole typed value.", BlueprintInternalUseOnly="true"))
	void SetMapFields_string(const TMap<FString, FString>& Fields);

	/** Set a map of fields with uint8 values */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field for each map entry, or Struct To Json for a whole typed value.", BlueprintInternalUseOnly="true"))
	void SetMapFields_uint8(const TMap<FString, uint8>& Fields);

	/** Set a map of fields with int32 values */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field for each map entry, or Struct To Json for a whole typed value.", BlueprintInternalUseOnly="true"))
	void SetMapFields_int32(const TMap<FString, int32>& Fields);

	/** Set a map of fields with int64 values */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field for each map entry, or Struct To Json for a whole typed value.", BlueprintInternalUseOnly="true"))
	void SetMapFields_int64(const TMap<FString, int64>& Fields);

	/** Set a map of fields with bool values */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field for each map entry, or Struct To Json for a whole typed value.", BlueprintInternalUseOnly="true"))
	void SetMapFields_bool(const TMap<FString, bool>& Fields);

private:
	/** Internal implementation for setting map fields. */
	template <typename T>
	void SetMapFields_Impl(const TMap<FString, T>& Fields)
	{
		for (auto& field : Fields)
		{
			// No need to support all int types as they're not supported by BP
			if (std::is_same_v<T, uint8> || std::is_same_v<T, int32> || std::is_same_v<T, int64> || std::is_same_v<T, float>)
			{
				SetNumberField(field.Key, field.Value);
			}
			else if (std::is_same_v<T, bool>)
			{
				SetBoolField(field.Key, (bool)field.Value);
			}
		}
	}

	/** Internal implementation to get number arrays of different types */
	template <typename T>
	TArray<T> GetTypeArrayField(const FString& FieldName) const
	{
		TArray<T> NumberArray;
		if (!JsonObj->HasTypedField<EJson::Array>(FieldName) || FieldName.IsEmpty())
		{
			UE_LOG(LogVaRest, Warning, TEXT("%s: No field with name %s of type Array"), *VA_FUNC_LINE, *FieldName);
			return NumberArray;
		}

		const TArray<TSharedPtr<FJsonValue>> JsonArrayValues = JsonObj->GetArrayField(FieldName);
		for (TArray<TSharedPtr<FJsonValue>>::TConstIterator It(JsonArrayValues); It; ++It)
		{
			const auto Value = (*It).Get();
			if (Value->Type != EJson::Number)
			{
				UE_LOG(LogVaRest, Error, TEXT("Not Number element in array with field name %s"), *FieldName);
			}

			NumberArray.Add((*It)->AsNumber());
		}

		return NumberArray;
	}

	//////////////////////////////////////////////////////////////////////////
	// Array fields helpers (uniform arrays)

public:
	/** Get the field named FieldName as a Number Array. Use it only if you're sure that array is uniform!
	 * Attn.!! float used instead of double to make the function blueprintable! */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	TArray<float> GetNumberArrayField(const FString& FieldName) const;

	/** Get the field named FieldName as a Number Array. Use it only if you're sure that array is uniform! */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	TArray<int32> GetIntegerArrayField(const FString& FieldName) const;

	/** DEPRECATED Attn.!! float used instead of double to make the function blueprintable! */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	void SetNumberArrayField(const FString& FieldName, const TArray<float>& NumberArray);

	/** Set an ObjectField named FieldName and value of Number Array */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	void SetNumberArrayFieldDouble(const FString& FieldName, const TArray<double>& NumberArray);

	/** Get the field named FieldName as a String Array. Use it only if you're sure that array is uniform! */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	TArray<FString> GetStringArrayField(const FString& FieldName) const;

	/** Set an ObjectField named FieldName and value of String Array */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	void SetStringArrayField(const FString& FieldName, const TArray<FString>& StringArray);

	/** Get the field named FieldName as a Bool Array. Use it only if you're sure that array is uniform! */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	TArray<bool> GetBoolArrayField(const FString& FieldName) const;

	/** Set an ObjectField named FieldName and value of Bool Array */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	void SetBoolArrayField(const FString& FieldName, const TArray<bool>& BoolArray);

	/** Get the field named FieldName as an Object Array. Use it only if you're sure that array is uniform! */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Get Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	TArray<UVaRestJsonObject*> GetObjectArrayField(const FString& FieldName) const;

	/** Set an ObjectField named FieldName and value of Ob Array */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Set Field with the corresponding value/array pin type.", BlueprintInternalUseOnly="true"))
	void SetObjectArrayField(const FString& FieldName, const TArray<UVaRestJsonObject*>& ObjectArray);

	//////////////////////////////////////////////////////////////////////////
	// Deserialize

public:
	/** Deserialize byte content to json */
	int32 DeserializeFromUTF8Bytes(const ANSICHAR* Bytes, int32 Size);

	/** Deserialize byte content to json */
	int32 DeserializeFromTCHARBytes(const TCHAR* Bytes, int32 Size);

	/** Deserialize byte stream from reader */
	void DecodeFromArchive(TUniquePtr<FArchive>& Reader);

	//////////////////////////////////////////////////////////////////////////
	// Serialize

public:
	/** Save json to file */
	bool WriteToFile(const FString& Path) const;

	/**
	 * Blueprint Save json to filepath
	 *
	 * @param bIsRelativeToProjectDir If set to 'false' path is treated as absolute
	 */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta=(DeprecatedFunction, DeprecationMessage="Use v2 Save Json File (VaRest Json).", BlueprintInternalUseOnly="true"))
	bool WriteToFilePath(const FString& Path, const bool bIsRelativeToProjectDir = true);

	static bool WriteStringToArchive(FArchive& Ar, const TCHAR* StrPtr, int64 Len);

	//////////////////////////////////////////////////////////////////////////
	// Data

private:
	/** Internal JSON data */
	TSharedRef<FJsonObject> JsonObj;
};
