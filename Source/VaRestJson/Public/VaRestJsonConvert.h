// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Templates/Function.h"
#include "UObject/Class.h"
#include "VaRestJson.h"

#include <initializer_list>

#include "VaRestJsonConvert.generated.h"

class FProperty;

/** How struct field names become JSON keys (spec 2.4). Decode accepts every policy's spelling regardless. */
UENUM(BlueprintType)
enum class EVaRestJsonNaming : uint8
{
	/**
	 * The authored field name unchanged: the C++ property name, or a Blueprint struct field's name. A C++ name is
	 * spelled the way a packaged game has it, where an FName keeps the casing first registered: a field named like an
	 * engine name in another casing comes out in the engine's (id -> ID, name -> Name), in the editor too. For an exact
	 * key register a rename (FVaRestJsonNames); the other policies normalise the case of each word anyway.
	 */
	AsIs,
	/** userId (the default) */
	CamelCase,
	/** user_id */
	SnakeCase,
	/** UserId */
	PascalCase
};

/**
 * Encode: offered every TArray<uint8> value (a field, or bytes inside a struct in a container) before the built-in
 * forms (an array of numbers; a base64 string with bBytesAsBase64). Return true after setting OutReplacement: the
 * encoder writes OutReplacement in place of the bytes through the same sink (RapidJSON writer or DOM builder),
 * counting its depth. Return false for the built-in forms. Called in walk order (field declaration order, then
 * container order), so a caller that numbers the values gets a deterministic numbering; empty arrays are offered too.
 * Socket.IO writes {"_placeholder":true,"num":N} and keeps a copy of Bytes (spec §18.2 J-1). Runs on the encoding thread.
 */
using FVaRestJsonBytesWriter = TFunction<bool(TConstArrayView<uint8> Bytes, FVaRestJson& OutReplacement)>;

struct FVaRestJsonConvertOptions
{
	EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase;

	/** Decode: lenient coercions ("42" -> 42, 1 -> true) are refused and every mismatch is an error */
	bool bStrict = false;

	/** Encode enums as their numeric value instead of the enumerator name (decode accepts both) */
	bool bEnumsAsNumbers = false;

	/** Encode TArray<uint8> as one base64 string instead of an array of numbers (decode accepts both) */
	bool bBytesAsBase64 = false;

	/** Encode: per-value replacement for TArray<uint8> values (see FVaRestJsonBytesWriter); unset: the built-in forms */
	FVaRestJsonBytesWriter BytesWriter;
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
 * A rename is an FString and keeps its case: it is how a C++ field whose name collides with an engine FName in another
 * casing (id is written ID, name is written Name) gets an exact key.
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
	// ---- encode: straight to UTF-8 bytes with the RapidJSON writer, no DOM, no FString (spec 2.4)

	/**
	 * Appends Struct's JSON to Out. False when a value nests deeper than VaRest::Json::MaxDepth: Out is left as it was
	 * and OutError is filled (or, without OutError, an error is logged).
	 */
	static bool StructToUtf8(const UScriptStruct* Struct, const void* Data, TArray<uint8>& Out,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed,
		FVaRestJsonError* OutError = nullptr);

	/** StructToUtf8 as an FString; "" plus an error log on failure */
	static FString StructToString(const UScriptStruct* Struct, const void* Data,
		const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed);

	/** Struct as a DOM (to Set into another document); an invalid handle plus an error log on failure */
	static FVaRestJson StructToJson(const UScriptStruct* Struct, const void* Data, const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions());

	/** One property's value as a DOM; Value points at the value, not at its container (the BP wildcard nodes' encode) */
	static FVaRestJson PropertyToJson(const FProperty* Property, const void* Value, const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions());

	template <typename T>
	static TArray<uint8> ToUtf8(const T& Value, const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions(), EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed)
	{
		TArray<uint8> Out;
		StructToUtf8(TBaseStructure<T>::Get(), &Value, Out, Options, Format);
		return Out;
	}

	template <typename T>
	static FVaRestJson ToJson(const T& Value, const FVaRestJsonConvertOptions& Options = FVaRestJsonConvertOptions())
	{
		return StructToJson(TBaseStructure<T>::Get(), &Value, Options);
	}

	/** Drops every cached field map. The editor calls it when a Blueprint struct changes; FVaRestJsonNames calls it too. */
	static void ResetCache();

	/** For tests: how many (struct, naming policy) field maps are cached */
	static int32 NumCachedFieldMapsInternal();
};
