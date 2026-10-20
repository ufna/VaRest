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

	/**
	 * Reads a UTF-8 JSON file (a BOM is skipped). Synchronous: the game thread waits while the whole file is read and
	 * parsed. A relative Path is not relative to the project (on desktop it starts at the executable's folder): build it
	 * from Get Project Saved Directory or another absolute folder.
	 */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Load Json File"))
	static UPARAM(DisplayName = "Success") bool LoadJsonFile(const FString& Path, FVaRestJson& Json, FVaRestJsonError& Error);

	/**
	 * Writes Json as UTF-8 without a BOM, replacing the file in place. Synchronous: the game thread waits while the
	 * whole file is written. A relative Path is not relative to the project, as in Load Json File. False when Json is
	 * invalid or the file cannot be written.
	 */
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

	/**
	 * Sets every member of Source on Target and returns Target: Target itself is changed, so every copy of its handle
	 * sees the new members. Each value is copied in; a key Target already has gets Source's value (an object there is
	 * replaced, not merged key by key).
	 */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Merge Json"))
	static FVaRestJson MergeJson(const FVaRestJson& Target, const FVaRestJson& Source);

	/** Elements of an array, members of an object, 0 otherwise */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Length (Json)"))
	static int32 JsonLength(const FVaRestJson& Json);

	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Array Remove At"))
	static UPARAM(DisplayName = "Removed") bool ArrayRemoveAt(const FVaRestJson& Json, int32 Index);

	/**
	 * An independent deep copy: changes to it never reach Json. Impure, so one call makes one copy that every connected
	 * node shares; a pure node would run once per connected node and hand each a different copy.
	 */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Json", meta = (DisplayName = "Clone (Json)"))
	static FVaRestJson CloneJson(const FVaRestJson& Json);

	/** Deep JSON equality (1 == 1.0, member order ignored) */
	UFUNCTION(BlueprintPure, Category = "VaRest|Json", meta = (DisplayName = "Equals (Json)"))
	static bool JsonEquals(const FVaRestJson& A, const FVaRestJson& B);

	// ---- wildcard nodes (CustomThunk, like the engine's JsonBlueprintFunctionLibrary.h:34-46). The int32 in each
	// signature is a placeholder: CustomStructureParam makes the pin a wildcard that takes the connected type
	// (K2Node_CallFunction.cpp:96-170), and the thunk reads the real property from the VM stack.

	/**
	 * The field converted to the connected type. Found is false, and Value the type's default, when the key is missing,
	 * null, or does not convert. A Json pin is the exception: it gets the field itself, a handle into the same JSON (Set
	 * Field on it changes Json), and a null field is found as JSON null.
	 * Pure, so it runs again for every node it is connected to: into a type with Json members (a struct with a VaRest
	 * Json field), each of those nodes decodes its own copy of them, and a Set Field on one copy is not seen by another.
	 * Store the result in a variable and read that instead.
	 */
	UFUNCTION(BlueprintPure, CustomThunk, Category = "VaRest|Json", meta = (CustomStructureParam = "Value", DisplayName = "Get Field"))
	static UPARAM(DisplayName = "Found") bool GetField(const FVaRestJson& Json, const FString& Key, int32& Value);
	DECLARE_FUNCTION(execGetField);

	/**
	 * Sets Key on the object Json to Value (any type) and returns Json. A connected invalid Json handle removes the key
	 * (spec 2.2); a value that cannot be encoded leaves the key as it was. Struct values use camelCase field names
	 * (Struct To Json has a Naming input).
	 */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "VaRest|Json", meta = (CustomStructureParam = "Value", AutoCreateRefTerm = "Value", DisplayName = "Set Field"))
	static FVaRestJson SetField(const FVaRestJson& Json, const FString& Key, const int32& Value);
	DECLARE_FUNCTION(execSetField);

	/** Get Field by path: "a.b[2].c", or an RFC 6901 pointer starting with '/'. The same Found, Json-pin and pure-node rules. */
	UFUNCTION(BlueprintPure, CustomThunk, Category = "VaRest|Json", meta = (CustomStructureParam = "Value", DisplayName = "Get Path"))
	static UPARAM(DisplayName = "Found") bool GetPath(const FVaRestJson& Json, const FString& Path, int32& Value);
	DECLARE_FUNCTION(execGetPath);

	/** Get Field by array index. The same Found, Json-pin and pure-node rules. */
	UFUNCTION(BlueprintPure, CustomThunk, Category = "VaRest|Json", meta = (CustomStructureParam = "Value", DisplayName = "Array Get"))
	static UPARAM(DisplayName = "Found") bool ArrayGet(const FVaRestJson& Json, int32 Index, int32& Value);
	DECLARE_FUNCTION(execArrayGet);

	/** Appends Value (any type) to the array Json and returns Json; nothing when Value cannot be encoded. Structs use camelCase field names. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "VaRest|Json", meta = (CustomStructureParam = "Value", AutoCreateRefTerm = "Value", DisplayName = "Array Add"))
	static FVaRestJson ArrayAdd(const FVaRestJson& Json, const int32& Value);
	DECLARE_FUNCTION(execArrayAdd);

	/**
	 * Value (a struct, an array of structs, any type) as JSON: a new document per call. Impure, so one call gives one
	 * result that every connected node shares. A connected Json value is returned as itself, the same handle and not a
	 * copy: Set Field on the result changes it (use Clone (Json) for a copy).
	 */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "VaRest|Json", meta = (CustomStructureParam = "Value", AutoCreateRefTerm = "Value", DisplayName = "Struct To Json"))
	static FVaRestJson StructToJson(const int32& Value, EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase);
	DECLARE_FUNCTION(execStructToJson);

	/**
	 * Fills Value (any type) from Json: tolerant names; strict refuses coercions. Value starts from the type's defaults
	 * on every call, so fields Json lacks get the defaults, never an earlier call's values. Success is false when Json
	 * does not fit, and Value may then be partly filled. A Json pin, or a Json field of a struct, gets its own copy of
	 * the JSON (Get Field hands a Json pin the handle itself instead).
	 */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "VaRest|Json", meta = (CustomStructureParam = "Value", DisplayName = "Json To Struct"))
	static UPARAM(DisplayName = "Success") bool JsonToStruct(const FVaRestJson& Json, int32& Value, EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase, bool bStrict = false);
	DECLARE_FUNCTION(execJsonToStruct);

	UFUNCTION(BlueprintPure, CustomThunk, Category = "VaRest|Json", meta = (CustomStructureParam = "Value", AutoCreateRefTerm = "Value", DisplayName = "Struct To Json String"))
	static FString StructToJsonString(const int32& Value, EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase, bool bPretty = false);
	DECLARE_FUNCTION(execStructToJsonString);

	/** Make Json node expansion (K2Node_VaRestMakeJson): a fresh object, pure so the expanded chain stays pure */
	UFUNCTION(BlueprintPure, meta = (BlueprintInternalUseOnly = "true"))
	static FVaRestJson MakeJsonInternalObject();

	/** Make Json node expansion: Set Field, pure */
	UFUNCTION(BlueprintPure, CustomThunk, meta = (BlueprintInternalUseOnly = "true", CustomStructureParam = "Value", AutoCreateRefTerm = "Value"))
	static FVaRestJson MakeJsonInternalSet(const FVaRestJson& Json, const FString& Key, const int32& Value);
	DECLARE_FUNCTION(execMakeJsonInternalSet);

	// ---- the wildcard nodes' work, callable from C++: Property/Address describe the connected value

	static bool GetFieldImpl(const FVaRestJson& Json, const FString& Key, const FProperty* Property, void* Address);
	static FVaRestJson SetFieldImpl(const FVaRestJson& Json, const FString& Key, const FProperty* Property, const void* Address);
	static bool GetPathImpl(const FVaRestJson& Json, const FString& Path, const FProperty* Property, void* Address);
	static bool ArrayGetImpl(const FVaRestJson& Json, int32 Index, const FProperty* Property, void* Address);
	static FVaRestJson ArrayAddImpl(const FVaRestJson& Json, const FProperty* Property, const void* Address);
	static FVaRestJson StructToJsonImpl(const FProperty* Property, const void* Address, EVaRestJsonNaming Naming);
	static bool JsonToStructImpl(const FVaRestJson& Json, const FProperty* Property, void* Address, EVaRestJsonNaming Naming, bool bStrict);
	static FString StructToJsonStringImpl(const FProperty* Property, const void* Address, EVaRestJsonNaming Naming, bool bPretty);
};
