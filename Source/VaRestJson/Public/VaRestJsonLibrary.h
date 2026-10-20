// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"

#include "VaRestJsonLibrary.generated.h"

/**
 * The Blueprint JSON library (spec 2.5). A VaRest Json value is a HANDLE: copying one aliases the same JSON, so Set
 * Field on any copy changes it everywhere (like VaRest 1 objects); Clone makes an independent copy. Make Json Object and
 * Make Json Array are impure on purpose: a pure maker would hand every consumer a fresh object.
 */
UCLASS()
class VARESTJSON_API UVaRestJsonLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// ---- text and files

	/** Parses JSON text. On failure Json is invalid and Error says where (line and column). */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Parse Json"))
	static UPARAM(DisplayName = "Success") bool ParseJson(const FString& Text, FVaRestJson& Json, FVaRestJsonError& Error);

	/** JSON text of Json; "" for an invalid handle */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "To String (Json)"))
	static FString JsonToString(const FVaRestJson& Json, bool bPretty = false);

	/** Reads a UTF-8 JSON file (a BOM is skipped) */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Load Json File"))
	static UPARAM(DisplayName = "Success") bool LoadJsonFile(const FString& Path, FVaRestJson& Json, FVaRestJsonError& Error);

	/** Writes Json as UTF-8 without a BOM */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Save Json File"))
	static UPARAM(DisplayName = "Success") bool SaveJsonFile(const FVaRestJson& Json, const FString& Path, bool bPretty = false);

	// ---- making values

	/** A new empty JSON object. Impure: every call makes a new one. */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Make Json Object"))
	static FVaRestJson MakeJsonObject();

	/** A new empty JSON array. Impure: every call makes a new one. */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Make Json Array"))
	static FVaRestJson MakeJsonArray();

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Json From Bool"))
	static FVaRestJson JsonFromBool(bool Value);

	/** An integral value within +-2^53 is stored as an integer, so 3 is written 3 (as JSON.stringify does) */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Json From Number"))
	static FVaRestJson JsonFromNumber(double Value);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Json From String"))
	static FVaRestJson JsonFromString(const FString& Value);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Json Null"))
	static FVaRestJson JsonNull();

	// ---- type

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Get Json Type"))
	static EVaRestJsonType GetJsonType(const FVaRestJson& Json);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Is Valid (Json)"))
	static bool IsValidJson(const FVaRestJson& Json);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Is Null (Json)"))
	static bool IsNullJson(const FVaRestJson& Json);

	// ---- typed getters: the field's value, or Default when it is missing or of another type

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Get String Field"))
	static FString GetStringField(const FVaRestJson& Json, const FString& Key, const FString& Default);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Get Int Field"))
	static int32 GetIntField(const FVaRestJson& Json, const FString& Key, int32 Default);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Get Int64 Field"))
	static int64 GetInt64Field(const FVaRestJson& Json, const FString& Key, int64 Default);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Get Number Field"))
	static double GetNumberField(const FVaRestJson& Json, const FString& Key, double Default);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Get Bool Field"))
	static bool GetBoolField(const FVaRestJson& Json, const FString& Key, bool Default);

	/** The object field itself (a handle into the same JSON), or Default */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Get Object Field", AutoCreateRefTerm = "Default"))
	static FVaRestJson GetObjectField(const FVaRestJson& Json, const FString& Key, const FVaRestJson& Default);

	/** The array field itself (a handle into the same JSON), or Default */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Get Array Field", AutoCreateRefTerm = "Default"))
	static FVaRestJson GetArrayField(const FVaRestJson& Json, const FString& Key, const FVaRestJson& Default);

	// ---- objects and arrays

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Get Keys (Json)"))
	static TArray<FString> GetJsonKeys(const FVaRestJson& Json);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Has Field"))
	static bool HasField(const FVaRestJson& Json, const FString& Key);

	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Remove Field"))
	static UPARAM(DisplayName = "Removed") bool RemoveField(const FVaRestJson& Json, const FString& Key);

	/** Copies every member of Source into Target (shallow, later wins) and returns Target */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Merge Json"))
	static FVaRestJson MergeJson(const FVaRestJson& Target, const FVaRestJson& Source);

	/** Elements of an array, members of an object, 0 otherwise */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Length (Json)"))
	static int32 JsonLength(const FVaRestJson& Json);

	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Array Remove At"))
	static UPARAM(DisplayName = "Removed") bool ArrayRemoveAt(const FVaRestJson& Json, int32 Index);

	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Clone (Json)"))
	static FVaRestJson CloneJson(const FVaRestJson& Json);

	/** Deep JSON equality (1 == 1.0, member order ignored) */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Equals (Json)"))
	static bool JsonEquals(const FVaRestJson& A, const FVaRestJson& B);
};
