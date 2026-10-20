// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Class.h"
#include "VaRestJson.h"

#include <initializer_list>

#include "VaRestJsonConvert.generated.h"

class FProperty;

/** How struct field names become JSON keys (spec 2.4). Decode accepts every policy's spelling regardless. */
UENUM(BlueprintType)
enum class EVaRestJsonNaming : uint8
{
	/** The authored field name unchanged: the C++ property name, or a Blueprint struct field's name */
	AsIs,
	/** userId (the default) */
	CamelCase,
	/** user_id */
	SnakeCase,
	/** UserId */
	PascalCase
};

struct FVaRestJsonConvertOptions
{
	EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase;

	/** Decode: lenient coercions ("42" -> 42, 1 -> true) are refused and every mismatch is an error */
	bool bStrict = false;

	/** Encode enums as their numeric value instead of the enumerator name (decode accepts both) */
	bool bEnumsAsNumbers = false;

	/** Encode TArray<uint8> as one base64 string instead of an array of numbers (decode accepts both) */
	bool bBytesAsBase64 = false;
};

/** What a decode found, each entry with the JSON path of the value ("items[3].name"). Strict mode reports Errors. */
struct FVaRestJsonDecodeReport
{
	TArray<FVaRestJsonError> Warnings;
	TArray<FVaRestJsonError> Errors;
};

/** A C++ field rename: Field is the property name, Json the key it is written as (no naming policy applies to it) */
struct FVaRestJsonRename
{
	FName Field;
	FString Json;
};

/**
 * C++ per-field renames (spec 2.4): FVaRestJsonNames::Register<FMyStruct>({ { TEXT("UserId"), TEXT("user_id") } }).
 * UPROPERTY meta is editor-only data, so names cannot come from meta in a cooked game; register at startup instead.
 */
struct VARESTJSON_API FVaRestJsonNames
{
	static void Register(const UScriptStruct* Struct, TConstArrayView<FVaRestJsonRename> Renames);

	template <typename T>
	static void Register(std::initializer_list<FVaRestJsonRename> Renames)
	{
		Register(TBaseStructure<T>::Get(), TConstArrayView<FVaRestJsonRename>(Renames.begin(), static_cast<int32>(Renames.size())));
	}

	static void Unregister(const UScriptStruct* Struct);
};

/**
 * Struct <-> JSON (spec 2.4). Every function may run on any thread. The struct's reflection must not change meanwhile:
 * a Blueprint struct recompiled in the editor while a worker thread decodes into it frees the FProperty objects that
 * decode is reading (UserDefinedStructureCompilerUtils.cpp:221-225), and dropping the cache cannot help a decode
 * already in flight. Any reflection user shares that hazard; it exists only in the editor.
 */
struct VARESTJSON_API FVaRestJsonConvert
{
	/** Drops every cached field map. The editor calls it when a Blueprint struct changes; FVaRestJsonNames calls it too. */
	static void ResetCache();

	/** For tests: how many (struct, naming policy) field maps are cached */
	static int32 NumCachedFieldMapsInternal();
};
